#include "gui/OccViewWidget.h"

#include <AIS_SelectionScheme.hxx>
#include <Aspect_Handle.hxx>
#include <Aspect_TypeOfLine.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <Graphic3d_GraphicDriver.hxx>
#include <Graphic3d_NameOfMaterial.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Poly_Triangle.hxx>
#include <Poly_Triangulation.hxx>
#include <Prs3d_LineAspect.hxx>
#include <Quantity_Color.hxx>
#include <SelectMgr_EntityOwner.hxx>
#include <Standard_Failure.hxx>
#include <StdSelect_BRepOwner.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopoDS_Shape.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>

#include <algorithm>
#include <array>
#include <QContextMenuEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEngine>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>
#include <QWheelEvent>

#include <cmath>
#include <exception>
#include <string>
#include <vector>

namespace spo {

namespace {

constexpr std::size_t kMaxSourceStlOverlayTriangles = 250000;
constexpr std::size_t kMaxCroppedStlOverlayTriangles = 500000;

TopoDS_Shape shapeFromOwner(const Handle(SelectMgr_EntityOwner)& owner) {
    const auto brepOwner = Handle(StdSelect_BRepOwner)::DownCast(owner);
    if (brepOwner.IsNull()) {
        return {};
    }
    return brepOwner->Shape();
}

Quantity_Color candidateColor(MergeCandidateType type, std::size_t index) {
    const auto shade = static_cast<double>(index % 3) * 0.08;
    switch (type) {
    case MergeCandidateType::PlaneLike:
        return Quantity_Color(1.00, 0.62 + shade, 0.05, Quantity_TOC_RGB);
    case MergeCandidateType::CylinderLike:
        return Quantity_Color(0.05, 0.38 + shade, 1.00, Quantity_TOC_RGB);
    case MergeCandidateType::SphereLike:
        return Quantity_Color(0.00, 0.72 + shade, 0.28, Quantity_TOC_RGB);
    case MergeCandidateType::ConeLike:
        return Quantity_Color(0.72 + shade, 0.12, 0.95, Quantity_TOC_RGB);
    case MergeCandidateType::TorusLike:
        return Quantity_Color(0.00, 0.78 + shade, 0.85, Quantity_TOC_RGB);
    case MergeCandidateType::FeatureBoundedRefit:
        return Quantity_Color(0.95, 0.95, 0.10 + shade, Quantity_TOC_RGB);
    case MergeCandidateType::FreeformG1:
        return Quantity_Color(0.36, 0.52 + shade, 0.68, Quantity_TOC_RGB);
    case MergeCandidateType::FreeformG2:
        return Quantity_Color(0.42, 0.66 + shade, 0.48, Quantity_TOC_RGB);
    case MergeCandidateType::SameDomain:
    case MergeCandidateType::Unknown:
        return Quantity_Color(0.55 + shade, 0.55 + shade, 0.55 + shade, Quantity_TOC_RGB);
    }
    return Quantity_Color(0.55, 0.55, 0.55, Quantity_TOC_RGB);
}

std::string occtMessage(const Standard_Failure& error) {
    const auto* message = error.GetMessageString();
    return message != nullptr ? message : "unknown OCCT error";
}

gp_Pnt pointFromStl(const StlVec3& point) {
    return gp_Pnt(point.x, point.y, point.z);
}

Handle(Poly_Triangulation) makeTriangulation(
    const StlMesh& mesh,
    std::size_t maxTriangles,
    std::size_t& displayedTriangleCount) {
    displayedTriangleCount = 0;
    const auto totalTriangles = mesh.triangleCount();
    if (totalTriangles == 0 || maxTriangles == 0) {
        return {};
    }

    const auto sampleStep = std::max<std::size_t>(1, (totalTriangles + maxTriangles - 1) / maxTriangles);
    const auto displayTriangles = (totalTriangles + sampleStep - 1) / sampleStep;
    auto triangulation = new Poly_Triangulation(
        static_cast<Standard_Integer>(displayTriangles * 3),
        static_cast<Standard_Integer>(displayTriangles),
        Standard_False,
        Standard_False);

    Standard_Integer nodeIndex = 1;
    Standard_Integer triangleIndex = 1;
    const auto& triangles = mesh.triangles();
    for (std::size_t index = 0; index < triangles.size(); index += sampleStep) {
        const auto& triangle = triangles[index];
        triangulation->SetNode(nodeIndex, pointFromStl(triangle.v0));
        triangulation->SetNode(nodeIndex + 1, pointFromStl(triangle.v1));
        triangulation->SetNode(nodeIndex + 2, pointFromStl(triangle.v2));
        triangulation->SetTriangle(triangleIndex, Poly_Triangle(nodeIndex, nodeIndex + 1, nodeIndex + 2));
        nodeIndex += 3;
        ++triangleIndex;
        ++displayedTriangleCount;
    }

    triangulation->UpdateCachedMinMax();
    return triangulation;
}

TopoDS_Shape makeBoundingBoxShape(const StlBoundingBox& bbox) {
    if (!bbox.valid) {
        return {};
    }

    const auto x0 = bbox.min.x;
    const auto y0 = bbox.min.y;
    const auto z0 = bbox.min.z;
    const auto x1 = bbox.max.x;
    const auto y1 = bbox.max.y;
    const auto z1 = bbox.max.z;

    const std::array<gp_Pnt, 8> points = {
        gp_Pnt(x0, y0, z0),
        gp_Pnt(x1, y0, z0),
        gp_Pnt(x1, y1, z0),
        gp_Pnt(x0, y1, z0),
        gp_Pnt(x0, y0, z1),
        gp_Pnt(x1, y0, z1),
        gp_Pnt(x1, y1, z1),
        gp_Pnt(x0, y1, z1)
    };
    constexpr std::array<std::pair<int, int>, 12> edges = {{
        {0, 1}, {1, 2}, {2, 3}, {3, 0},
        {4, 5}, {5, 6}, {6, 7}, {7, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7}
    }};

    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    for (const auto& edge : edges) {
        builder.Add(compound, BRepBuilderAPI_MakeEdge(points[edge.first], points[edge.second]).Edge());
    }
    return compound;
}

}

OccViewWidget::OccViewWidget(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_DontCreateNativeAncestors);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
}

OccViewWidget::~OccViewWidget() {
    if (!context_.IsNull()) {
        context_->RemoveAll(Standard_False);
    }
    mergeCandidateShapes_.clear();
    displayedShape_.Nullify();
    hoverShape_.Nullify();
    featureEdgeShape_.Nullify();
    lockedEdgeShape_.Nullify();
    sourceStlShape_.Nullify();
    croppedStlShape_.Nullify();
    stlCropBoxShape_.Nullify();
    patchOverlayShape_.Nullify();
    context_.Nullify();
    view_.Nullify();
    viewer_.Nullify();
    window_.Nullify();
    displayConnection_.Nullify();
}

QPaintEngine* OccViewWidget::paintEngine() const {
    return context_.IsNull() ? QWidget::paintEngine() : nullptr;
}

Result OccViewWidget::displayDocument(const ShapeDocument& document) {
    try {
        initializeOcct();
        hasDocument_ = document.hasShape();
        stats_ = document.stats();
        topology_ = &document.topology();
        selectedFaces_.clear();
        selectedEdges_.clear();
        lastSelectedFace_ = -1;
        lastSelectedEdge_ = -1;

        if (context_.IsNull()) {
            return Result::ok();
        }

        context_->RemoveAll(Standard_False);
        hoverShape_.Nullify();
        featureEdgeShape_.Nullify();
        lockedEdgeShape_.Nullify();
        sourceStlShape_.Nullify();
        croppedStlShape_.Nullify();
        stlCropBoxShape_.Nullify();
        patchCutoutPreviewShape_.Nullify();
        patchOverlayShape_.Nullify();
        sourceStlDisplayedTriangleCount_ = 0;
        croppedStlDisplayedTriangleCount_ = 0;
        mergeCandidateShapes_.clear();
        mergeCandidateFaceColors_.clear();
        mergePreviewVisible_ = false;
        displayedShape_ = new AIS_ColoredShape(document.shape());
        displayedShape_->SetMaterial(Graphic3d_NOM_PLASTIC);
        displayedShape_->SetColor(Quantity_Color(1.0, 0.74, 0.16, Quantity_TOC_RGB));
        displayedShape_->SetDisplayMode(AIS_Shaded);
        displayedShape_->Attributes()->SetFaceBoundaryDraw(Standard_True);
        displayedShape_->Attributes()->SetFaceBoundaryAspect(
            new Prs3d_LineAspect(Quantity_Color(0.08, 0.08, 0.08, Quantity_TOC_RGB), Aspect_TOL_SOLID, 1.0));

        context_->Display(displayedShape_, Standard_False);
        context_->UpdateCurrentViewer();
        activateSelectionMode();
        resizeOcctWindow();
        fitAll();
        return Result::ok();
    } catch (const Standard_Failure& error) {
        clearDocument();
        return Result::error(std::string("OCCT 显示模型异常：") + occtMessage(error));
    } catch (const std::exception& error) {
        clearDocument();
        return Result::error(std::string("显示模型异常：") + error.what());
    } catch (...) {
        clearDocument();
        return Result::error("显示模型异常：未知错误");
    }
}

void OccViewWidget::clearDocument() {
    hasDocument_ = false;
    stats_ = {};
    topology_ = nullptr;
    selectedFaces_.clear();
    selectedEdges_.clear();
    lastSelectedFace_ = -1;
    lastSelectedEdge_ = -1;

    if (!context_.IsNull()) {
        context_->RemoveAll(Standard_False);
    }

    displayedShape_.Nullify();
    hoverShape_.Nullify();
    featureEdgeShape_.Nullify();
    lockedEdgeShape_.Nullify();
    sourceStlShape_.Nullify();
    croppedStlShape_.Nullify();
    stlCropBoxShape_.Nullify();
    patchCutoutPreviewShape_.Nullify();
    patchOverlayShape_.Nullify();
    sourceStlDisplayedTriangleCount_ = 0;
    croppedStlDisplayedTriangleCount_ = 0;
    mergeCandidateShapes_.clear();
    mergeCandidateFaceColors_.clear();
    mergePreviewVisible_ = false;
    redrawView();
}

void OccViewWidget::setSelectionMode(SelectionMode mode) {
    selectionMode_ = mode;
    if (mode == SelectionMode::Face) {
        selectedEdges_.clear();
        lastSelectedEdge_ = -1;
    } else if (mode == SelectionMode::Edge) {
        selectedFaces_.clear();
        lastSelectedFace_ = -1;
    } else {
        selectedFaces_.clear();
        selectedEdges_.clear();
        lastSelectedFace_ = -1;
        lastSelectedEdge_ = -1;
    }
    redrawSelectedShapes();
    activateSelectionMode();
}

void OccViewWidget::showFeatureEdges(const FeatureEdgeDetectionResult& result) {
    if (context_.IsNull() || topology_ == nullptr) {
        return;
    }

    clearFeatureEdgeShape();
    if (result.edges.empty()) {
        redrawView();
        return;
    }

    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    for (const auto& edge : result.edges) {
        builder.Add(compound, topology_->edge(edge.edge));
    }

    featureEdgeShape_ = new AIS_Shape(compound);
    featureEdgeShape_->SetDisplayMode(AIS_WireFrame);
    featureEdgeShape_->SetColor(Quantity_Color(1.0, 0.05, 0.05, Quantity_TOC_RGB));
    featureEdgeShape_->SetWidth(3.0);
    context_->Display(featureEdgeShape_, Standard_False);
    context_->UpdateCurrentViewer();
    redrawView();
}

void OccViewWidget::showLockedEdges(const std::set<EdgeId>& edgeIds) {
    if (context_.IsNull() || topology_ == nullptr) {
        return;
    }

    clearLockedEdgeShape();
    if (edgeIds.empty()) {
        redrawView();
        return;
    }

    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    for (const auto edgeId : edgeIds) {
        if (edgeId < topology_->edgeCount()) {
            builder.Add(compound, topology_->edge(edgeId));
        }
    }

    lockedEdgeShape_ = new AIS_Shape(compound);
    lockedEdgeShape_->SetDisplayMode(AIS_WireFrame);
    lockedEdgeShape_->SetColor(Quantity_Color(0.90, 0.0, 1.0, Quantity_TOC_RGB));
    lockedEdgeShape_->SetWidth(5.0);
    context_->Display(lockedEdgeShape_, Standard_False);
    context_->UpdateCurrentViewer();
    redrawView();
}

void OccViewWidget::showMergeCandidates(
    const std::vector<MergeCandidate>& candidates,
    int maxCandidatesToShow,
    bool showAll) {
    if (context_.IsNull() || topology_ == nullptr) {
        return;
    }

    clearMergeCandidates();
    if (candidates.empty()) {
        return;
    }

    std::vector<const MergeCandidate*> sortedCandidates;
    sortedCandidates.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        if (candidate.valid &&
            candidate.status != MergeCandidateStatus::Hidden &&
            !candidate.faces.empty()) {
            sortedCandidates.push_back(&candidate);
        }
    }

    std::sort(sortedCandidates.begin(), sortedCandidates.end(), [](const auto* lhs, const auto* rhs) {
        if (lhs->face_count != rhs->face_count) {
            return lhs->face_count > rhs->face_count;
        }
        return lhs->total_area > rhs->total_area;
    });

    const auto displayCount = showAll
        ? sortedCandidates.size()
        : std::min<std::size_t>(sortedCandidates.size(), static_cast<std::size_t>(std::max(0, maxCandidatesToShow)));

    for (std::size_t index = 0; index < displayCount; ++index) {
        const auto* candidate = sortedCandidates[index];
        BRep_Builder builder;
        TopoDS_Compound compound;
        builder.MakeCompound(compound);
        bool hasFace = false;
        for (const auto faceId : candidate->faces) {
            if (faceId < topology_->faceCount()) {
                builder.Add(compound, topology_->face(faceId));
                hasFace = true;
            }
        }
        if (!hasFace) {
            continue;
        }

        const auto color = candidateColor(candidate->candidate_type, index);
        for (const auto faceId : candidate->faces) {
            if (faceId < topology_->faceCount()) {
                mergeCandidateFaceColors_.push_back({faceId, color});
            }
        }

        Handle(AIS_Shape) boundaryOverlay = new AIS_Shape(compound);
        boundaryOverlay->SetDisplayMode(AIS_WireFrame);
        boundaryOverlay->SetColor(color);
        boundaryOverlay->SetWidth(4.0);
        context_->Display(boundaryOverlay, Standard_False);
        context_->Deactivate(boundaryOverlay);
        mergeCandidateShapes_.push_back(boundaryOverlay);
    }

    applyCustomAspects();
    context_->UpdateCurrentViewer();
    redrawView();
}

bool OccViewWidget::showMergeCandidateById(const std::vector<MergeCandidate>& candidates, int candidateId) {
    const auto it = std::find_if(candidates.begin(), candidates.end(), [candidateId](const auto& candidate) {
        return candidate.candidate_id == candidateId;
    });
    if (it == candidates.end()) {
        clearMergeCandidates();
        return false;
    }

    showMergeCandidates(std::vector<MergeCandidate>{*it}, 1, true);
    return true;
}

void OccViewWidget::clearMergeCandidates() {
    if (!context_.IsNull()) {
        for (const auto& shape : mergeCandidateShapes_) {
            if (!shape.IsNull()) {
                context_->Remove(shape, Standard_False);
            }
        }
        context_->UpdateCurrentViewer();
    }
    mergeCandidateShapes_.clear();
    mergeCandidateFaceColors_.clear();
    applyCustomAspects();
    redrawView();
}

void OccViewWidget::showSourceStl(const StlMesh& mesh) {
    initializeOcct();
    clearSourceStl();
    if (context_.IsNull() || mesh.empty()) {
        return;
    }

    std::size_t displayedTriangles = 0;
    const auto triangulation = makeTriangulation(mesh, kMaxSourceStlOverlayTriangles, displayedTriangles);
    if (triangulation.IsNull()) {
        return;
    }

    sourceStlShape_ = new AIS_Triangulation(triangulation);
    sourceStlShape_->SetColor(Quantity_Color(0.20, 0.60, 1.00, Quantity_TOC_RGB));
    sourceStlShape_->SetTransparency(0.72);
    sourceStlShape_->SetDisplayMode(AIS_Shaded);
    context_->Display(sourceStlShape_, Standard_False);
    context_->Deactivate(sourceStlShape_);
    sourceStlDisplayedTriangleCount_ = displayedTriangles;
    if (!sourceStlVisible_) {
        context_->Erase(sourceStlShape_, Standard_False);
    }
    context_->UpdateCurrentViewer();
    redrawView();
}

void OccViewWidget::clearSourceStl() {
    if (!context_.IsNull() && !sourceStlShape_.IsNull()) {
        context_->Remove(sourceStlShape_, Standard_False);
        context_->UpdateCurrentViewer();
    }
    sourceStlShape_.Nullify();
    sourceStlDisplayedTriangleCount_ = 0;
    redrawView();
}

void OccViewWidget::showCroppedStl(const StlMesh& mesh) {
    initializeOcct();
    clearCroppedStl();
    if (context_.IsNull() || mesh.empty()) {
        return;
    }

    std::size_t displayedTriangles = 0;
    const auto triangulation = makeTriangulation(mesh, kMaxCroppedStlOverlayTriangles, displayedTriangles);
    if (triangulation.IsNull()) {
        return;
    }

    croppedStlShape_ = new AIS_Triangulation(triangulation);
    croppedStlShape_->SetColor(Quantity_Color(0.00, 0.95, 0.80, Quantity_TOC_RGB));
    croppedStlShape_->SetTransparency(0.18);
    croppedStlShape_->SetDisplayMode(AIS_Shaded);
    context_->Display(croppedStlShape_, Standard_False);
    context_->Deactivate(croppedStlShape_);
    croppedStlDisplayedTriangleCount_ = displayedTriangles;
    if (!croppedStlVisible_) {
        context_->Erase(croppedStlShape_, Standard_False);
    }
    context_->UpdateCurrentViewer();
    redrawView();
}

void OccViewWidget::clearCroppedStl() {
    if (!context_.IsNull() && !croppedStlShape_.IsNull()) {
        context_->Remove(croppedStlShape_, Standard_False);
        context_->UpdateCurrentViewer();
    }
    croppedStlShape_.Nullify();
    croppedStlDisplayedTriangleCount_ = 0;
    redrawView();
}

void OccViewWidget::showStlCropBox(const StlBoundingBox& bbox) {
    initializeOcct();
    clearStlCropBox();
    if (context_.IsNull() || !bbox.valid) {
        return;
    }

    const auto shape = makeBoundingBoxShape(bbox);
    if (shape.IsNull()) {
        return;
    }

    stlCropBoxShape_ = new AIS_Shape(shape);
    stlCropBoxShape_->SetDisplayMode(AIS_WireFrame);
    stlCropBoxShape_->SetColor(Quantity_Color(0.00, 1.00, 0.20, Quantity_TOC_RGB));
    stlCropBoxShape_->SetWidth(3.0);
    context_->Display(stlCropBoxShape_, Standard_False);
    context_->Deactivate(stlCropBoxShape_);
    if (!stlCropBoxVisible_) {
        context_->Erase(stlCropBoxShape_, Standard_False);
    }
    context_->UpdateCurrentViewer();
    redrawView();
}

void OccViewWidget::clearStlCropBox() {
    if (!context_.IsNull() && !stlCropBoxShape_.IsNull()) {
        context_->Remove(stlCropBoxShape_, Standard_False);
        context_->UpdateCurrentViewer();
    }
    stlCropBoxShape_.Nullify();
    redrawView();
}

void OccViewWidget::showPatchCutoutPreview(const std::vector<FaceId>& hiddenFaces) {
    initializeOcct();
    clearPatchCutoutPreview();
    if (context_.IsNull() || topology_ == nullptr || displayedShape_.IsNull()) {
        return;
    }

    std::set<FaceId> hidden(hiddenFaces.begin(), hiddenFaces.end());
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    bool hasVisibleFace = false;
    for (FaceId faceId = 0; faceId < topology_->faceCount(); ++faceId) {
        if (hidden.contains(faceId)) {
            continue;
        }
        builder.Add(compound, topology_->face(faceId));
        hasVisibleFace = true;
    }

    context_->Erase(displayedShape_, Standard_False);
    if (hasVisibleFace) {
        patchCutoutPreviewShape_ = new AIS_Shape(compound);
        patchCutoutPreviewShape_->SetDisplayMode(AIS_Shaded);
        patchCutoutPreviewShape_->SetMaterial(Graphic3d_NOM_PLASTIC);
        patchCutoutPreviewShape_->SetColor(Quantity_Color(1.0, 0.74, 0.16, Quantity_TOC_RGB));
        patchCutoutPreviewShape_->Attributes()->SetFaceBoundaryDraw(Standard_True);
        patchCutoutPreviewShape_->Attributes()->SetFaceBoundaryAspect(
            new Prs3d_LineAspect(Quantity_Color(0.08, 0.08, 0.08, Quantity_TOC_RGB), Aspect_TOL_SOLID, 1.0));
        context_->Display(patchCutoutPreviewShape_, Standard_False);
        context_->Deactivate(patchCutoutPreviewShape_);
    }
    context_->UpdateCurrentViewer();
    redrawView();
}

void OccViewWidget::showPatchOverlay(const TopoDS_Shape& patchShape) {
    initializeOcct();
    clearPatchOverlayShape();
    if (context_.IsNull() || patchShape.IsNull()) {
        return;
    }

    patchOverlayShape_ = new AIS_Shape(patchShape);
    patchOverlayShape_->SetDisplayMode(AIS_Shaded);
    patchOverlayShape_->SetColor(Quantity_Color(0.0, 0.95, 0.35, Quantity_TOC_RGB));
    patchOverlayShape_->SetTransparency(0.35);
    patchOverlayShape_->Attributes()->SetFaceBoundaryDraw(Standard_True);
    patchOverlayShape_->Attributes()->SetFaceBoundaryAspect(
        new Prs3d_LineAspect(Quantity_Color(0.0, 0.25, 0.08, Quantity_TOC_RGB), Aspect_TOL_SOLID, 2.0));
    context_->Display(patchOverlayShape_, Standard_False);
    context_->Deactivate(patchOverlayShape_);
    context_->UpdateCurrentViewer();
    redrawView();
}

void OccViewWidget::clearPatchOverlayShape() {
    if (!context_.IsNull() && !patchOverlayShape_.IsNull()) {
        context_->Remove(patchOverlayShape_, Standard_False);
        context_->UpdateCurrentViewer();
    }
    patchOverlayShape_.Nullify();
}

void OccViewWidget::clearPatchCutoutPreview() {
    if (!context_.IsNull() && !patchCutoutPreviewShape_.IsNull()) {
        context_->Remove(patchCutoutPreviewShape_, Standard_False);
        patchCutoutPreviewShape_.Nullify();
        if (!displayedShape_.IsNull()) {
            context_->Display(displayedShape_, Standard_False);
        }
        context_->UpdateCurrentViewer();
    } else {
        patchCutoutPreviewShape_.Nullify();
    }
}

void OccViewWidget::clearPatchOverlay() {
    clearPatchOverlayShape();
    clearPatchCutoutPreview();
    redrawView();
}

bool OccViewWidget::hasPatchOverlay() const {
    return !patchOverlayShape_.IsNull();
}

void OccViewWidget::setSourceStlVisible(bool visible) {
    sourceStlVisible_ = visible;
    if (!context_.IsNull() && !sourceStlShape_.IsNull()) {
        if (visible) {
            context_->Display(sourceStlShape_, Standard_False);
            context_->Deactivate(sourceStlShape_);
        } else {
            context_->Erase(sourceStlShape_, Standard_False);
        }
        context_->UpdateCurrentViewer();
    }
    redrawView();
}

void OccViewWidget::setCroppedStlVisible(bool visible) {
    croppedStlVisible_ = visible;
    if (!context_.IsNull() && !croppedStlShape_.IsNull()) {
        if (visible) {
            context_->Display(croppedStlShape_, Standard_False);
            context_->Deactivate(croppedStlShape_);
        } else {
            context_->Erase(croppedStlShape_, Standard_False);
        }
        context_->UpdateCurrentViewer();
    }
    redrawView();
}

void OccViewWidget::setStlCropBoxVisible(bool visible) {
    stlCropBoxVisible_ = visible;
    if (!context_.IsNull() && !stlCropBoxShape_.IsNull()) {
        if (visible) {
            context_->Display(stlCropBoxShape_, Standard_False);
            context_->Deactivate(stlCropBoxShape_);
        } else {
            context_->Erase(stlCropBoxShape_, Standard_False);
        }
        context_->UpdateCurrentViewer();
    }
    redrawView();
}

std::size_t OccViewWidget::sourceStlDisplayedTriangleCount() const {
    return sourceStlDisplayedTriangleCount_;
}

std::size_t OccViewWidget::croppedStlDisplayedTriangleCount() const {
    return croppedStlDisplayedTriangleCount_;
}

void OccViewWidget::setFeatureLinesVisible(bool visible) {
    featureLinesVisible_ = visible;
    if (!context_.IsNull() && !displayedShape_.IsNull()) {
        displayedShape_->Attributes()->SetFaceBoundaryDraw(visible);
        context_->Redisplay(displayedShape_, Standard_False);
    }
    redrawView();
}

void OccViewWidget::setMergePreviewVisible(bool visible) {
    mergePreviewVisible_ = visible;
    if (!visible) {
        clearMergeCandidates();
    }
    if (!context_.IsNull() && !displayedShape_.IsNull()) {
        displayedShape_->SetTransparency(0.0);
        context_->Redisplay(displayedShape_, Standard_False);
    }
    redrawView();
}

void OccViewWidget::resetView() {
    initializeOcct();
    if (!view_.IsNull()) {
        view_->SetProj(V3d_XposYnegZpos);
        resizeOcctWindow();
        fitAll();
    }
}

void OccViewWidget::fitAll() {
    initializeOcct();
    if (!view_.IsNull()) {
        resizeOcctWindow();
        view_->SetImmediateUpdate(Standard_False);
        view_->FitAll(0.01, Standard_False);
        view_->ZFitAll();
        redrawView();
    }
}

void OccViewWidget::requestRedraw() {
    if (view_.IsNull()) {
        update();
        return;
    }

    resizeOcctWindow();
    redrawView();

    QTimer::singleShot(0, this, [this]() {
        resizeOcctWindow();
        redrawView();
    });
    QTimer::singleShot(50, this, [this]() {
        resizeOcctWindow();
        redrawView();
    });
    QTimer::singleShot(150, this, [this]() {
        resizeOcctWindow();
        redrawView();
    });
}

void OccViewWidget::setSelectionCallback(std::function<void(QString, QList<QPair<QString, QString>>)> callback) {
    selectionCallback_ = std::move(callback);
}

void OccViewWidget::setLockEdgesCallback(std::function<void(std::vector<EdgeId>)> callback) {
    lockEdgesCallback_ = std::move(callback);
}

void OccViewWidget::setUnlockEdgesCallback(std::function<void(std::vector<EdgeId>)> callback) {
    unlockEdgesCallback_ = std::move(callback);
}

void OccViewWidget::setCandidateFaceCallback(std::function<void(FaceId)> callback) {
    candidateFaceCallback_ = std::move(callback);
}

std::vector<FaceId> OccViewWidget::selectedFaceIds() const {
    return {selectedFaces_.begin(), selectedFaces_.end()};
}

std::vector<EdgeId> OccViewWidget::selectedEdgeIds() const {
    return {selectedEdges_.begin(), selectedEdges_.end()};
}

bool OccViewWidget::event(QEvent* event) {
    const bool redrawAfter =
        event->type() == QEvent::FocusIn ||
        event->type() == QEvent::FocusOut ||
        event->type() == QEvent::WindowActivate ||
        event->type() == QEvent::WindowDeactivate ||
        event->type() == QEvent::ActivationChange;

    const bool handled = QWidget::event(event);
    if (redrawAfter) {
        requestRedraw();
    }
    return handled;
}

void OccViewWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (hasDocument_) {
        initializeOcct();
        resizeOcctWindow();
        fitAll();
    } else {
        requestRedraw();
    }
}

void OccViewWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (!context_.IsNull()) {
        resizeOcctWindow();
        requestRedraw();
    }
}

void OccViewWidget::paintEvent(QPaintEvent*) {
    if (context_.IsNull()) {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);
        return;
    }
    redrawView();
}

void OccViewWidget::focusInEvent(QFocusEvent* event) {
    QWidget::focusInEvent(event);
    requestRedraw();
}

void OccViewWidget::focusOutEvent(QFocusEvent* event) {
    QWidget::focusOutEvent(event);
    requestRedraw();
}

void OccViewWidget::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Alt) {
        event->accept();
        requestRedraw();
        return;
    }
    QWidget::keyPressEvent(event);
}

void OccViewWidget::keyReleaseEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Alt) {
        event->accept();
        requestRedraw();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void OccViewWidget::mousePressEvent(QMouseEvent* event) {
    initializeOcct();
    lastMousePosition_ = event->pos();

    if (event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::AltModifier)) {
        panning_ = true;
        return;
    }
    if (event->button() == Qt::MiddleButton && !view_.IsNull()) {
        rotating_ = true;
        const auto point = nativePosition(event->position());
        view_->StartRotation(point.x(), point.y());
        return;
    }
    if (event->button() == Qt::RightButton) {
        panning_ = true;
        return;
    }
    if (event->button() == Qt::LeftButton) {
        selectAt(event->position(), event->modifiers());
        return;
    }

    QWidget::mousePressEvent(event);
}

void OccViewWidget::mouseMoveEvent(QMouseEvent* event) {
    initializeOcct();
    const auto delta = event->pos() - lastMousePosition_;
    lastMousePosition_ = event->pos();

    if (rotating_ && !view_.IsNull()) {
        const auto point = nativePosition(event->position());
        view_->Rotation(point.x(), point.y());
        redrawView();
        return;
    }

    if (panning_ && !view_.IsNull()) {
        const auto nativePan = nativeDelta(delta);
        view_->Pan(nativePan.x(), -nativePan.y());
        redrawView();
        return;
    }

    if (!context_.IsNull() && !view_.IsNull()) {
        updateHoverShape(event->position());
    }

    QWidget::mouseMoveEvent(event);
}

void OccViewWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton) {
        rotating_ = false;
        return;
    }
    if (event->button() == Qt::RightButton || event->button() == Qt::LeftButton) {
        panning_ = false;
        return;
    }

    QWidget::mouseReleaseEvent(event);
}

void OccViewWidget::wheelEvent(QWheelEvent* event) {
    initializeOcct();
    if (view_.IsNull()) {
        return;
    }

    const auto zoomFactor = event->angleDelta().y() > 0 ? 0.94 : 1.06;
    view_->SetZoom(zoomFactor);
    redrawView();
}

void OccViewWidget::contextMenuEvent(QContextMenuEvent* event) {
    QMenu menu(this);
    if (selectionMode_ == SelectionMode::Edge) {
        if (selectedEdges_.empty()) {
            selectAt(event->pos(), Qt::NoModifier);
        }

        auto* lockAction = menu.addAction("锁定选中边");
        auto* unlockAction = menu.addAction("解锁选中边");
        lockAction->setEnabled(!selectedEdges_.empty());
        unlockAction->setEnabled(!selectedEdges_.empty());
        connect(lockAction, &QAction::triggered, this, [this]() {
            if (lockEdgesCallback_) {
                lockEdgesCallback_(selectedEdgeIds());
            }
        });
        connect(unlockAction, &QAction::triggered, this, [this]() {
            if (unlockEdgesCallback_) {
                unlockEdgesCallback_(selectedEdgeIds());
            }
        });
    } else if (selectionMode_ == SelectionMode::Face) {
        menu.addAction("显示面信息");
        menu.addAction("加入合并组");
        menu.addAction("移出合并组");
        menu.addAction("预览合并区域");
    } else {
        menu.addAction("预览此候选区域");
        menu.addAction("应用此候选区域");
        menu.addAction("拒绝此候选区域");
        menu.addAction("显示候选区域统计");
    }
    menu.exec(event->globalPos());
}

QString OccViewWidget::selectionModeText() const {
    switch (selectionMode_) {
    case SelectionMode::Face:
        return "选择面";
    case SelectionMode::Edge:
        return "选择边";
    case SelectionMode::Candidate:
        return "候选区域";
    }
    return "选择面";
}

QPoint OccViewWidget::nativePosition(const QPointF& position) const {
    const auto scale = devicePixelRatioF();
    return {
        static_cast<int>(std::lround(position.x() * scale)),
        static_cast<int>(std::lround(position.y() * scale))
    };
}

QPoint OccViewWidget::nativeDelta(const QPoint& delta) const {
    const auto scale = devicePixelRatioF();
    return {
        static_cast<int>(std::lround(delta.x() * scale)),
        static_cast<int>(std::lround(delta.y() * scale))
    };
}

TopoDS_Shape OccViewWidget::detectedShapeAt(const QPointF& position) {
    if (context_.IsNull() || view_.IsNull()) {
        return {};
    }

    const auto point = nativePosition(position);
    context_->MoveTo(point.x(), point.y(), view_, Standard_False);
    TopoDS_Shape detectedShape = shapeFromOwner(context_->DetectedOwner());
    if (context_->HasDetected()) {
        const auto shape = context_->DetectedShape();
        if (!shape.IsNull()) {
            detectedShape = shape;
        }
    }
    context_->ClearDetected(Standard_False);
    return detectedShape;
}

void OccViewWidget::clearHoverShape() {
    if (!context_.IsNull() && !hoverShape_.IsNull()) {
        context_->Remove(hoverShape_, Standard_False);
    }
    hoverShape_.Nullify();
}

void OccViewWidget::clearFeatureEdgeShape() {
    if (!context_.IsNull() && !featureEdgeShape_.IsNull()) {
        context_->Remove(featureEdgeShape_, Standard_False);
    }
    featureEdgeShape_.Nullify();
}

void OccViewWidget::clearLockedEdgeShape() {
    if (!context_.IsNull() && !lockedEdgeShape_.IsNull()) {
        context_->Remove(lockedEdgeShape_, Standard_False);
    }
    lockedEdgeShape_.Nullify();
}

void OccViewWidget::updateHoverShape(const QPointF& position) {
    if (!hasDocument_ || topology_ == nullptr || context_.IsNull() || view_.IsNull()) {
        return;
    }

    const auto shape = detectedShapeAt(position);
    const bool isExpectedShape =
        (selectionMode_ == SelectionMode::Face && topology_->faceIdFor(shape).has_value()) ||
        (selectionMode_ == SelectionMode::Edge && topology_->edgeIdFor(shape).has_value()) ||
        (selectionMode_ == SelectionMode::Candidate && topology_->faceIdFor(shape).has_value());

    clearHoverShape();
    if (!isExpectedShape) {
        context_->UpdateCurrentViewer();
        redrawView();
        return;
    }

    hoverShape_ = new AIS_Shape(shape);
    hoverShape_->SetDisplayMode(AIS_WireFrame);
    hoverShape_->SetColor(Quantity_Color(0.0, 0.95, 0.95, Quantity_TOC_RGB));
    hoverShape_->SetWidth(2.0);
    context_->Display(hoverShape_, Standard_False);
    context_->UpdateCurrentViewer();
    redrawView();
}

void OccViewWidget::selectAt(const QPointF& position, Qt::KeyboardModifiers modifiers) {
    if (!hasDocument_ || topology_ == nullptr || context_.IsNull() || view_.IsNull()) {
        return;
    }

    const TopoDS_Shape selectedShape = detectedShapeAt(position);
    context_->ClearSelected(Standard_False);
    const bool multiSelect = modifiers.testFlag(Qt::ShiftModifier);
    const bool removeSelect = modifiers.testFlag(Qt::ControlModifier);

    if (selectionMode_ == SelectionMode::Face) {
        const auto faceId = topology_->faceIdFor(selectedShape);
        if (!faceId.has_value()) {
            return;
        }

        clearHoverShape();
        if (!multiSelect && !removeSelect) {
            selectedFaces_.clear();
        }
        if (removeSelect || (multiSelect && selectedFaces_.contains(*faceId))) {
            selectedFaces_.erase(*faceId);
        } else {
            selectedFaces_.insert(*faceId);
        }
        selectedEdges_.clear();
        lastSelectedFace_ = static_cast<int>(*faceId);
        lastSelectedEdge_ = -1;
        redrawSelectedShapes();

        if (selectionCallback_) {
            const auto faceEdges = topology_->edgesForFace(*faceId);
            const auto adjacentFaces = topology_->adjacentFaces(*faceId);
            selectionCallback_("面", {
                {"选择模式", selectionModeText()},
                {"Face ID", selectedFaces_.size() == 1 ? QString::number(*faceId) : "多选"},
                {"已选面数量", QString::number(selectedFaces_.size())},
                {"最近命中 ID", QString::number(*faceId)},
                {"关联边数量", QString::number(faceEdges.size())},
                {"相邻面数量", QString::number(adjacentFaces.size())}
            });
        }
    } else if (selectionMode_ == SelectionMode::Edge) {
        const auto edgeId = topology_->edgeIdFor(selectedShape);
        if (!edgeId.has_value()) {
            return;
        }

        clearHoverShape();
        if (!multiSelect && !removeSelect) {
            selectedEdges_.clear();
        }
        if (removeSelect || (multiSelect && selectedEdges_.contains(*edgeId))) {
            selectedEdges_.erase(*edgeId);
        } else {
            selectedEdges_.insert(*edgeId);
        }
        selectedFaces_.clear();
        lastSelectedFace_ = -1;
        lastSelectedEdge_ = static_cast<int>(*edgeId);
        redrawSelectedShapes();

        if (selectionCallback_) {
            const auto* adjacency = topology_->adjacencyForEdge(*edgeId);
            selectionCallback_("边", {
                {"选择模式", selectionModeText()},
                {"Edge ID", selectedEdges_.size() == 1 ? QString::number(*edgeId) : "多选"},
                {"已选边数量", QString::number(selectedEdges_.size())},
                {"最近命中 ID", QString::number(*edgeId)},
                {"邻接面数量", adjacency != nullptr ? QString::number(adjacency->faces.size()) : "0"},
                {"边界类型", adjacency != nullptr && adjacency->faces.size() == 1 ? "自由边" : "内部边"}
            });
        }
    } else {
        const auto faceId = topology_->faceIdFor(selectedShape);
        if (!faceId.has_value()) {
            return;
        }

        clearHoverShape();
        selectedFaces_.clear();
        selectedEdges_.clear();
        lastSelectedFace_ = static_cast<int>(*faceId);
        lastSelectedEdge_ = -1;

        if (candidateFaceCallback_) {
            candidateFaceCallback_(*faceId);
        }
    }
}

void OccViewWidget::redrawSelectedShapes() {
    if (context_.IsNull() || displayedShape_.IsNull() || topology_ == nullptr) {
        return;
    }

    applyCustomAspects();
    redrawView();
}

void OccViewWidget::applyCustomAspects() {
    if (context_.IsNull() || displayedShape_.IsNull() || topology_ == nullptr) {
        return;
    }

    displayedShape_->ClearCustomAspects();
    for (const auto& [faceId, color] : mergeCandidateFaceColors_) {
        if (faceId < topology_->faceCount()) {
            displayedShape_->SetCustomColor(topology_->face(faceId), color);
            displayedShape_->SetCustomTransparency(topology_->face(faceId), 0.0);
        }
    }
    for (const auto faceId : selectedFaces_) {
        if (faceId < topology_->faceCount()) {
            displayedShape_->SetCustomColor(
                topology_->face(faceId),
                Quantity_Color(0.15, 0.80, 0.20, Quantity_TOC_RGB));
            displayedShape_->SetCustomTransparency(topology_->face(faceId), 0.0);
        }
    }
    for (const auto edgeId : selectedEdges_) {
        if (edgeId < topology_->edgeCount()) {
            displayedShape_->SetCustomColor(
                topology_->edge(edgeId),
                Quantity_Color(0.15, 0.80, 0.20, Quantity_TOC_RGB));
            displayedShape_->SetCustomWidth(topology_->edge(edgeId), 3.0);
        }
    }

    context_->Redisplay(displayedShape_, Standard_False);
    context_->UpdateCurrentViewer();
}

void OccViewWidget::initializeOcct() {
    if (!context_.IsNull()) {
        return;
    }

    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_OpaquePaintEvent);

    displayConnection_ = new Aspect_DisplayConnection();
    Handle(Graphic3d_GraphicDriver) graphicDriver = new OpenGl_GraphicDriver(displayConnection_);
    viewer_ = new V3d_Viewer(graphicDriver);
    viewer_->SetDefaultLights();
    viewer_->SetLightOn();

    context_ = new AIS_InteractiveContext(viewer_);
    context_->SetDisplayMode(AIS_Shaded, Standard_False);
    context_->SetAutoActivateSelection(Standard_False);
    context_->SetAutomaticHilight(Standard_False);
    context_->SetToHilightSelected(Standard_False);

    view_ = viewer_->CreateView();
    window_ = new WNT_Window(reinterpret_cast<Aspect_Handle>(winId()));
    view_->SetWindow(window_);
    if (!window_->IsMapped()) {
        window_->Map();
    }
    view_->SetBackgroundColor(Quantity_Color(0.44, 0.48, 0.52, Quantity_TOC_RGB));
    resizeOcctWindow();
}

void OccViewWidget::activateSelectionMode() {
    if (context_.IsNull() || displayedShape_.IsNull()) {
        return;
    }

    context_->Deactivate(displayedShape_);
    context_->Load(displayedShape_, -1, Standard_False);
    if (selectionMode_ == SelectionMode::Edge) {
        context_->Activate(displayedShape_, AIS_Shape::SelectionMode(TopAbs_EDGE));
    } else {
        context_->Activate(displayedShape_, AIS_Shape::SelectionMode(TopAbs_FACE));
    }
    context_->UpdateCurrentViewer();
    redrawView();
}

void OccViewWidget::resizeOcctWindow() {
    if (!window_.IsNull()) {
        window_->DoResize();
    }
    if (!view_.IsNull()) {
        view_->MustBeResized();
    }
}

void OccViewWidget::redrawView() {
    if (!view_.IsNull()) {
        view_->Redraw();
    }
}

}
