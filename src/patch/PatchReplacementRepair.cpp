#include "patch/PatchReplacementRepair.h"

#include "validate/ShapeValidator.h"

#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepLib.hxx>
#include <BRepTools_ReShape.hxx>
#include <Bnd_Box.hxx>
#include <ShapeFix_Face.hxx>
#include <ShapeFix_Shape.hxx>
#include <ShapeFix_Shell.hxx>
#include <ShapeFix_Solid.hxx>
#include <ShapeFix_Wire.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <tuple>
#include <vector>

namespace spo {

namespace {

struct RepairStats {
    int faces = 0;
    int edges = 0;
    int shells = 0;
    int solids = 0;
    int freeEdges = 0;
    int multipleEdges = 0;
    bool brepCheckValid = false;
};

struct SewingAttempt {
    TopoDS_Shape shape;
    double tolerance = 0.0;
    RepairStats stats;
    bool collapsed = false;
    bool shellToSolidApplied = false;
};

std::string occt_message(const Standard_Failure& error) {
    const auto* message = error.GetMessageString();
    if (message == nullptr || std::string(message).empty()) {
        return "unknown OCCT failure";
    }
    return message;
}

void append_repair_warning(PatchReplacementRepairReport& report, const std::string& warning) {
    if (warning.empty()) {
        return;
    }
    if (!report.warningMessage.empty()) {
        report.warningMessage += " ";
    }
    report.warningMessage += warning;
}

int count_shapes(const TopoDS_Shape& shape, TopAbs_ShapeEnum type) {
    int count = 0;
    if (shape.IsNull()) {
        return count;
    }
    for (TopExp_Explorer explorer(shape, type); explorer.More(); explorer.Next()) {
        ++count;
    }
    return count;
}

RepairStats capture_stats(const TopoDS_Shape& shape) {
    RepairStats stats;
    const ShapeDocument document(shape, {});
    const auto validation = ShapeValidator().validate(document);
    stats.faces = validation.stats.faces;
    stats.edges = validation.stats.edges;
    stats.shells = validation.stats.shells;
    stats.solids = validation.stats.solids;
    stats.freeEdges = validation.free_edges;
    stats.multipleEdges = validation.multiple_edges;
    stats.brepCheckValid = validation.brep_check_valid;
    return stats;
}

void copy_before_stats(PatchReplacementRepairReport& report, const RepairStats& stats) {
    report.faceCountBeforeRepair = stats.faces;
    report.edgeCountBeforeRepair = stats.edges;
    report.shellCountBeforeRepair = stats.shells;
    report.solidCountBeforeRepair = stats.solids;
    report.freeEdgesBeforeRepair = stats.freeEdges;
    report.multipleEdgesBeforeRepair = stats.multipleEdges;
}

void copy_after_stats(PatchReplacementRepairReport& report, const RepairStats& stats) {
    report.faceCountAfterRepair = stats.faces;
    report.edgeCountAfterRepair = stats.edges;
    report.shellCountAfterRepair = stats.shells;
    report.solidCountAfterRepair = stats.solids;
    report.freeEdgesAfterRepair = stats.freeEdges;
    report.multipleEdgesAfterRepair = stats.multipleEdges;
}

void copy_best_sewing_stats(PatchReplacementRepairReport& report, const SewingAttempt& attempt) {
    report.selectedSewingTolerance = attempt.tolerance;
    report.bestSewingFreeEdges = attempt.stats.freeEdges;
    report.bestSewingMultipleEdges = attempt.stats.multipleEdges;
    report.bestSewingFaceCount = attempt.stats.faces;
    report.bestSewingEdgeCount = attempt.stats.edges;
    report.bestSewingShellCount = attempt.stats.shells;
    report.bestSewingSolidCount = attempt.stats.solids;
    report.bestSewingBRepCheckValid = attempt.stats.brepCheckValid;
    report.bestSewingCollapsed = attempt.collapsed;
}

double clamped_tolerance(double value, const PatchReplacementRepairOptions& options) {
    const double minTolerance = std::max(1.0e-9, options.minSewingTolerance);
    const double maxTolerance = std::max(minTolerance, options.maxSewingTolerance);
    return std::max(minTolerance, std::min(value, maxTolerance));
}

double bbox_diagonal(const TopoDS_Shape& shape) {
    Bnd_Box box;
    BRepBndLib::Add(shape, box);
    if (box.IsVoid()) {
        return 100.0;
    }

    double xmin = 0.0;
    double ymin = 0.0;
    double zmin = 0.0;
    double xmax = 0.0;
    double ymax = 0.0;
    double zmax = 0.0;
    box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    const double dx = xmax - xmin;
    const double dy = ymax - ymin;
    const double dz = zmax - zmin;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

std::vector<double> sewing_tolerances(
    const TopoDS_Shape& shape,
    const PatchReplacementRepairOptions& options) {
    const double baseTolerance = clamped_tolerance(bbox_diagonal(shape) * 1.0e-4, options);
    std::vector<double> tolerances = {
        baseTolerance,
        clamped_tolerance(baseTolerance * 2.0, options),
        clamped_tolerance(baseTolerance * 5.0, options),
        clamped_tolerance(baseTolerance * 10.0, options),
        clamped_tolerance(options.preferredSewingTolerance, options),
        clamped_tolerance(options.maxSewingTolerance * 0.5, options),
        clamped_tolerance(options.maxSewingTolerance * 0.8, options),
        clamped_tolerance(options.maxSewingTolerance, options),
    };

    std::sort(tolerances.begin(), tolerances.end());
    tolerances.erase(
        std::unique(tolerances.begin(), tolerances.end(), [](double lhs, double rhs) {
            return std::abs(lhs - rhs) <= 1.0e-9;
        }),
        tolerances.end());
    return tolerances;
}

TopoDS_Shape run_shape_fix_shape(const TopoDS_Shape& shape, bool& applied) {
    if (shape.IsNull()) {
        return shape;
    }
    ShapeFix_Shape fixer(shape);
    fixer.Perform();
    const auto fixed = fixer.Shape();
    applied = true;
    return fixed.IsNull() ? shape : fixed;
}

void run_shape_fix_wire(const TopoDS_Shape& shape, double tolerance) {
    for (TopExp_Explorer faceExplorer(shape, TopAbs_FACE); faceExplorer.More(); faceExplorer.Next()) {
        const auto face = TopoDS::Face(faceExplorer.Current());
        for (TopExp_Explorer wireExplorer(face, TopAbs_WIRE); wireExplorer.More(); wireExplorer.Next()) {
            ShapeFix_Wire wireFixer;
            wireFixer.Load(TopoDS::Wire(wireExplorer.Current()));
            wireFixer.SetFace(face);
            wireFixer.SetPrecision(tolerance);
            wireFixer.FixReorder();
            wireFixer.FixConnected();
            wireFixer.FixClosed();
        }
    }
}

TopoDS_Shape run_shape_fix_face(const TopoDS_Shape& shape, double tolerance) {
    BRepTools_ReShape reshaper;
    bool replacedAnyFace = false;
    for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
        const auto face = TopoDS::Face(explorer.Current());
        ShapeFix_Face faceFixer(face);
        faceFixer.SetPrecision(tolerance);
        faceFixer.FixOrientation();
        faceFixer.Perform();
        const auto fixedFace = faceFixer.Face();
        if (!fixedFace.IsNull()) {
            reshaper.Replace(face, fixedFace);
            replacedAnyFace = true;
        }
    }
    if (!replacedAnyFace) {
        return shape;
    }
    const auto fixedShape = reshaper.Apply(shape);
    return fixedShape.IsNull() ? shape : fixedShape;
}

TopoDS_Shape run_unify_same_domain(const TopoDS_Shape& shape, bool& applied) {
    if (shape.IsNull()) {
        return shape;
    }
    try {
        ShapeUpgrade_UnifySameDomain unifier(shape, Standard_True, Standard_True, Standard_True);
        unifier.Build();
        const auto unified = unifier.Shape();
        applied = true;
        return unified.IsNull() ? shape : unified;
    } catch (...) {
        return shape;
    }
}

TopoDS_Shape run_shell_to_solid(const TopoDS_Shape& shape, bool& applied) {
    if (shape.IsNull() || count_shapes(shape, TopAbs_SOLID) > 0) {
        return shape;
    }

    for (TopExp_Explorer explorer(shape, TopAbs_SHELL); explorer.More(); explorer.Next()) {
        try {
            const auto shell = TopoDS::Shell(explorer.Current());
            ShapeFix_Shell shellFixer(shell);
            shellFixer.Perform();
            const auto fixedShell = shellFixer.Shell();
            if (fixedShell.IsNull()) {
                continue;
            }

            BRepBuilderAPI_MakeSolid solidBuilder(fixedShell);
            if (!solidBuilder.IsDone()) {
                continue;
            }
            const auto solid = solidBuilder.Solid();
            if (!solid.IsNull()) {
                applied = true;
                return solid;
            }
        } catch (...) {
        }
    }

    return shape;
}

TopoDS_Shape run_shape_fix_solid(const TopoDS_Shape& shape, bool& applied) {
    if (shape.IsNull()) {
        return shape;
    }

    BRepTools_ReShape reshaper;
    bool replacedAnySolid = false;
    for (TopExp_Explorer explorer(shape, TopAbs_SOLID); explorer.More(); explorer.Next()) {
        try {
            const auto solid = TopoDS::Solid(explorer.Current());
            ShapeFix_Solid solidFixer(solid);
            solidFixer.Perform();
            const auto fixedSolid = solidFixer.Solid();
            if (!fixedSolid.IsNull()) {
                reshaper.Replace(solid, fixedSolid);
                replacedAnySolid = true;
            }
        } catch (...) {
        }
    }

    if (!replacedAnySolid) {
        return shape;
    }
    const auto fixedShape = reshaper.Apply(shape);
    applied = true;
    return fixedShape.IsNull() ? shape : fixedShape;
}

SewingAttempt run_sewing_attempt(
    const TopoDS_Shape& shape,
    double tolerance,
    const PatchReplacementRepairOptions& options,
    int minKeepFaceCount) {
    SewingAttempt attempt;
    attempt.tolerance = tolerance;

    BRepBuilderAPI_Sewing sewing;
    sewing.SetTolerance(tolerance);
    sewing.SetNonManifoldMode(Standard_True);
    sewing.SetFloatingEdgesMode(Standard_True);
    sewing.SetSameParameterMode(Standard_True);
    sewing.Add(shape);
    sewing.Perform();

    auto sewedShape = sewing.SewedShape();
    if (options.runShellToSolid) {
        sewedShape = run_shell_to_solid(sewedShape, attempt.shellToSolidApplied);
    }

    bool shapeFixApplied = false;
    sewedShape = run_shape_fix_shape(sewedShape, shapeFixApplied);

    bool solidFixApplied = false;
    sewedShape = run_shape_fix_solid(sewedShape, solidFixApplied);

    bool unifyApplied = false;
    sewedShape = run_unify_same_domain(sewedShape, unifyApplied);

    attempt.shape = sewedShape;
    attempt.stats = capture_stats(sewedShape);
    attempt.collapsed = attempt.stats.faces < minKeepFaceCount;
    return attempt;
}

bool usable_sewing_result(const SewingAttempt& attempt) {
    return attempt.stats.brepCheckValid &&
        attempt.stats.solids > 0 &&
        !attempt.collapsed;
}

const SewingAttempt* choose_best_attempt(
    const std::vector<SewingAttempt>& attempts,
    double preferredTolerance,
    int inputFaceCount) {
    const SewingAttempt* preferred = nullptr;
    for (const auto& attempt : attempts) {
        if (usable_sewing_result(attempt) &&
            std::abs(attempt.tolerance - preferredTolerance) <= 1.0e-9) {
            preferred = &attempt;
            break;
        }
    }
    if (preferred != nullptr) {
        return preferred;
    }

    const SewingAttempt* bestUsable = nullptr;
    for (const auto& attempt : attempts) {
        if (!usable_sewing_result(attempt)) {
            continue;
        }
        if (bestUsable == nullptr ||
            std::make_tuple(
                attempt.stats.freeEdges,
                std::abs(attempt.tolerance - preferredTolerance),
                std::abs(attempt.stats.faces - inputFaceCount)) <
                std::make_tuple(
                    bestUsable->stats.freeEdges,
                    std::abs(bestUsable->tolerance - preferredTolerance),
                    std::abs(bestUsable->stats.faces - inputFaceCount))) {
            bestUsable = &attempt;
        }
    }
    if (bestUsable != nullptr) {
        return bestUsable;
    }

    const SewingAttempt* bestNonCollapsed = nullptr;
    for (const auto& attempt : attempts) {
        if (attempt.collapsed) {
            continue;
        }
        if (bestNonCollapsed == nullptr ||
            std::make_tuple(attempt.stats.freeEdges, -attempt.stats.solids) <
                std::make_tuple(bestNonCollapsed->stats.freeEdges, -bestNonCollapsed->stats.solids)) {
            bestNonCollapsed = &attempt;
        }
    }
    if (bestNonCollapsed != nullptr) {
        return bestNonCollapsed;
    }

    const SewingAttempt* leastFreeEdges = nullptr;
    for (const auto& attempt : attempts) {
        if (leastFreeEdges == nullptr ||
            attempt.stats.freeEdges < leastFreeEdges->stats.freeEdges) {
            leastFreeEdges = &attempt;
        }
    }
    return leastFreeEdges;
}

} // namespace

PatchReplacementRepairResult repairPatchReplacementShape(
    const TopoDS_Shape& inputShape,
    const PatchReplacementRepairOptions& options) {
    PatchReplacementRepairResult result;
    result.shape = inputShape;
    if (inputShape.IsNull()) {
        result.report.message = "Repair pipeline requires a non-empty shape.";
        return result;
    }

    try {
        copy_before_stats(result.report, capture_stats(result.shape));
        const int inputFaceCount = std::max(1, result.report.faceCountBeforeRepair);
        const int minKeepFaceCount = std::max(1, static_cast<int>(std::floor(
            static_cast<double>(inputFaceCount) * options.collapseFaceRatio)));

        if (options.runShapeFixShape) {
            result.shape = run_shape_fix_shape(result.shape, result.report.shapeFixShapeApplied);
        }
        if (options.runSameParameter) {
            BRepLib::SameParameter(result.shape, options.sewingTolerance, Standard_True);
            result.report.sameParameterApplied = true;
        }
        if (options.runShapeFixWire) {
            run_shape_fix_wire(result.shape, options.sewingTolerance);
            result.report.shapeFixWireApplied = true;
        }
        if (options.runShapeFixFace) {
            result.shape = run_shape_fix_face(result.shape, options.sewingTolerance);
            result.report.shapeFixFaceApplied = true;
        }
        if (options.runUnifySameDomain) {
            result.shape = run_unify_same_domain(result.shape, result.report.unifySameDomainApplied);
        }
        if (options.runShellToSolid) {
            result.shape = run_shell_to_solid(result.shape, result.report.shellToSolidApplied);
        }

        if (options.runSewing) {
            std::vector<double> tolerances;
            if (options.runAdaptiveSewing) {
                tolerances = sewing_tolerances(result.shape, options);
                result.report.adaptiveSewingApplied = true;
            } else {
                tolerances = {options.sewingTolerance};
            }

            std::vector<SewingAttempt> attempts;
            attempts.reserve(tolerances.size());
            for (const auto tolerance : tolerances) {
                attempts.push_back(run_sewing_attempt(
                    result.shape,
                    tolerance,
                    options,
                    minKeepFaceCount));
            }

            result.report.sewingApplied = true;
            result.report.sewingAttemptCount = static_cast<int>(attempts.size());
            if (!attempts.empty()) {
                const auto* best = choose_best_attempt(
                    attempts,
                    clamped_tolerance(options.preferredSewingTolerance, options),
                    inputFaceCount);
                if (best != nullptr) {
                    copy_best_sewing_stats(result.report, *best);
                    result.report.shellToSolidApplied = result.report.shellToSolidApplied || best->shellToSolidApplied;
                    if (usable_sewing_result(*best)) {
                        result.shape = best->shape;
                    } else {
                        append_repair_warning(
                            result.report,
                            "Best sewing result was kept as diagnostic only because no valid non-collapsed solid sewing result was found.");
                    }
                }
            }
        }

        if (options.runShellToSolid) {
            result.shape = run_shell_to_solid(result.shape, result.report.shellToSolidApplied);
        }
        bool solidFixApplied = false;
        result.shape = run_shape_fix_solid(result.shape, solidFixApplied);
        if (solidFixApplied) {
            result.report.shapeFixShapeApplied = true;
        }
        if (options.runUnifySameDomain) {
            bool finalUnifyApplied = false;
            result.shape = run_unify_same_domain(result.shape, finalUnifyApplied);
            result.report.unifySameDomainApplied = result.report.unifySameDomainApplied || finalUnifyApplied;
        }

        copy_after_stats(result.report, capture_stats(result.shape));
        result.success = !result.shape.IsNull();
        result.report.success = result.success;
        result.report.message = result.success
            ? "Patch replacement industrial repair pipeline completed."
            : "Patch replacement industrial repair pipeline produced an empty shape.";
        return result;
    } catch (const Standard_Failure& error) {
        result.report.message = std::string("Patch replacement industrial repair pipeline failed: ") + occt_message(error);
    } catch (const std::exception& error) {
        result.report.message = std::string("Patch replacement industrial repair pipeline failed: ") + error.what();
    } catch (...) {
        result.report.message = "Patch replacement industrial repair pipeline failed: unknown exception.";
    }

    result.success = false;
    result.report.success = false;
    return result;
}

} // namespace spo
