#include "patch/PatchImportService.h"

#include "io/StepReader.h"

#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <Bnd_Box.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <IGESControl_Reader.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace spo {

namespace {

std::string path_to_string(const std::filesystem::path& path) {
    const auto utf8Path = path.generic_u8string();
    return {reinterpret_cast<const char*>(utf8Path.c_str()), utf8Path.size()};
}

std::string path_to_occt_string(const std::filesystem::path& path) {
    const auto utf8Path = path.u8string();
    return {reinterpret_cast<const char*>(utf8Path.c_str()), utf8Path.size()};
}

std::string lowercase_extension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return extension;
}

bool is_step_extension(const std::string& extension) {
    return extension == ".stp" || extension == ".step";
}

bool is_iges_extension(const std::string& extension) {
    return extension == ".igs" || extension == ".iges";
}

ImportedPatchInfo fail(std::filesystem::path sourcePath, std::string message) {
    ImportedPatchInfo info;
    info.sourcePath = std::move(sourcePath);
    info.errorMessage = std::move(message);
    return info;
}

void count_shape_items(ImportedPatchInfo& info) {
    for (TopExp_Explorer explorer(info.shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
        ++info.faceCount;
    }
    for (TopExp_Explorer explorer(info.shape, TopAbs_EDGE); explorer.More(); explorer.Next()) {
        ++info.edgeCount;
    }
    for (TopExp_Explorer explorer(info.shape, TopAbs_SOLID); explorer.More(); explorer.Next()) {
        ++info.solidCount;
    }
    for (TopExp_Explorer explorer(info.shape, TopAbs_SHELL); explorer.More(); explorer.Next()) {
        ++info.shellCount;
    }
}

void compute_bbox(ImportedPatchInfo& info) {
    Bnd_Box box;
    BRepBndLib::Add(info.shape, box);
    if (box.IsVoid()) {
        return;
    }

    box.Get(
        info.bboxMinX,
        info.bboxMinY,
        info.bboxMinZ,
        info.bboxMaxX,
        info.bboxMaxY,
        info.bboxMaxZ);
    info.bboxValid = true;
}

ImportedPatchInfo make_success(std::filesystem::path sourcePath, TopoDS_Shape shape, std::string message) {
    ImportedPatchInfo info;
    info.success = true;
    info.message = std::move(message);
    info.sourcePath = std::move(sourcePath);
    info.shape = std::move(shape);

    count_shape_items(info);
    compute_bbox(info);

    BRepCheck_Analyzer analyzer(info.shape);
    info.brepCheckValid = analyzer.IsValid();

    return info;
}

ImportedPatchInfo import_step_patch(const std::filesystem::path& path) {
    const StepReader reader;
    const auto result = reader.read(path);
    if (!result.status.success()) {
        return fail(path, result.status.message());
    }
    if (!result.document.hasShape()) {
        return fail(path, "STEP/STP import produced an empty patch shape.");
    }
    return make_success(path, result.document.shape(), "Imported STEP/STP patch: " + path_to_string(path));
}

ImportedPatchInfo import_iges_patch(const std::filesystem::path& path) {
    try {
        IGESControl_Reader reader;
        const auto occtPath = path_to_occt_string(path);
        const auto status = reader.ReadFile(occtPath.c_str());
        if (status != IFSelect_RetDone) {
            return fail(path, "OCCT failed to read IGES patch file.");
        }

        const auto roots = reader.NbRootsForTransfer();
        if (roots <= 0) {
            return fail(path, "IGES patch file has no transferable roots.");
        }

        reader.TransferRoots();
        auto shape = reader.OneShape();
        if (shape.IsNull()) {
            return fail(path, "IGES import produced an empty patch shape.");
        }

        return make_success(path, std::move(shape), "Imported IGES patch: " + path_to_string(path));
    } catch (const Standard_Failure& error) {
        const auto* message = error.GetMessageString();
        return fail(path, std::string("OCCT IGES import exception: ") + (message != nullptr ? message : "unknown error"));
    } catch (const std::exception& error) {
        return fail(path, std::string("IGES import exception: ") + error.what());
    } catch (...) {
        return fail(path, "IGES import exception: unknown error.");
    }
}

std::string attempt_failure_message(
    const char* label,
    const std::filesystem::path& path,
    const ImportedPatchInfo& info) {
    std::string message = label;
    message += " ";
    message += path_to_string(path);
    message += ": ";
    message += !info.errorMessage.empty() ? info.errorMessage : info.message;
    return message;
}

std::string join_failures(const std::vector<std::string>& failures) {
    std::string joined;
    for (const auto& failure : failures) {
        if (!joined.empty()) {
            joined += " | ";
        }
        joined += failure;
    }
    if (joined.empty()) {
        joined = "No STEP/STP/IGES patch paths were provided.";
    }
    return joined;
}

}

ImportedPatchInfo PatchImportService::importPatch(const std::filesystem::path& patchPath) const {
    if (patchPath.empty()) {
        return fail(patchPath, "Patch path is empty.");
    }
    if (!std::filesystem::exists(patchPath)) {
        return fail(patchPath, "Patch file does not exist: " + path_to_string(patchPath));
    }

    const auto extension = lowercase_extension(patchPath);
    if (is_step_extension(extension)) {
        auto info = import_step_patch(patchPath);
        info.attemptedStepPath = patchPath;
        return info;
    }
    if (is_iges_extension(extension)) {
        auto info = import_iges_patch(patchPath);
        info.attemptedIgesPath = patchPath;
        return info;
    }

    return fail(patchPath, "Unsupported patch file extension: " + extension);
}

ImportedPatchInfo PatchImportService::importPatchFromResult(
    const GeomagicAutoSurfaceResult& result) const {
    std::filesystem::path attemptedStepPath;
    std::filesystem::path attemptedIgesPath;
    std::vector<std::string> failures;

    if (!result.outputStepPath.empty()) {
        attemptedStepPath = result.outputStepPath;
        auto info = importPatch(result.outputStepPath);
        info.attemptedStepPath = attemptedStepPath;
        info.attemptedIgesPath = attemptedIgesPath;
        if (info.success) {
            info.message = "Imported patch from result.outputStepPath: " + path_to_string(result.outputStepPath);
            return info;
        }
        failures.push_back(attempt_failure_message("result.outputStepPath", result.outputStepPath, info));
    }

    if (!result.outputIgesPath.empty()) {
        attemptedIgesPath = result.outputIgesPath;
        auto info = importPatch(result.outputIgesPath);
        info.attemptedStepPath = attemptedStepPath;
        info.attemptedIgesPath = attemptedIgesPath;
        if (info.success) {
            info.message = "Imported patch from result.outputIgesPath: " + path_to_string(result.outputIgesPath);
            return info;
        }
        failures.push_back(attempt_failure_message("result.outputIgesPath", result.outputIgesPath, info));
    }

    if (!result.preservedIgesPath.empty()) {
        attemptedIgesPath = result.preservedIgesPath;
        auto info = importPatch(result.preservedIgesPath);
        info.attemptedStepPath = attemptedStepPath;
        info.attemptedIgesPath = attemptedIgesPath;
        if (info.success) {
            info.message = "Imported patch from result.preservedIgesPath: " + path_to_string(result.preservedIgesPath);
            return info;
        }
        failures.push_back(attempt_failure_message("result.preservedIgesPath", result.preservedIgesPath, info));
    }

    ImportedPatchInfo info;
    info.attemptedStepPath = attemptedStepPath;
    info.attemptedIgesPath = attemptedIgesPath;
    info.errorMessage = join_failures(failures);
    return info;
}

}
