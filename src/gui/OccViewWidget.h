#pragma once

#include "brep/ShapeDocument.h"
#include "common/Result.h"
#include "feature/FeatureEdgeDetector.h"
#include "gui/GuiTypes.h"
#include "merge/MergeCandidate.h"
#include "stl/StlMesh.h"

#include <QPoint>
#include <QWidget>

#include <AIS_ColoredShape.hxx>
#include <AIS_InteractiveContext.hxx>
#include <AIS_Shape.hxx>
#include <AIS_Triangulation.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <Quantity_Color.hxx>
#include <TopoDS_Shape.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>
#include <WNT_Window.hxx>

#include <functional>
#include <utility>
#include <set>
#include <vector>

namespace spo {

class TopologyGraph;

class OccViewWidget final : public QWidget {
public:
    explicit OccViewWidget(QWidget* parent = nullptr);
    ~OccViewWidget() override;
    Result displayDocument(const ShapeDocument& document);
    void clearDocument();
    void setSelectionMode(SelectionMode mode);
    void showFeatureEdges(const FeatureEdgeDetectionResult& result);
    void showLockedEdges(const std::set<EdgeId>& edgeIds);
    void showMergeCandidates(
        const std::vector<MergeCandidate>& candidates,
        int maxCandidatesToShow = 10,
        bool showAll = false);
    bool showMergeCandidateById(const std::vector<MergeCandidate>& candidates, int candidateId);
    void clearMergeCandidates();
    void showSourceStl(const StlMesh& mesh);
    void clearSourceStl();
    void showCroppedStl(const StlMesh& mesh);
    void clearCroppedStl();
    void showStlCropBox(const StlBoundingBox& bbox);
    void clearStlCropBox();
    void showPatchCutoutPreview(const std::vector<FaceId>& hiddenFaces);
    void showPatchOverlay(const TopoDS_Shape& patchShape);
    void clearPatchOverlay();
    bool hasPatchOverlay() const;
    void setSourceStlVisible(bool visible);
    void setCroppedStlVisible(bool visible);
    void setStlCropBoxVisible(bool visible);
    std::size_t sourceStlDisplayedTriangleCount() const;
    std::size_t croppedStlDisplayedTriangleCount() const;
    void setFeatureLinesVisible(bool visible);
    void setMergePreviewVisible(bool visible);
    void resetView();
    void fitAll();
    void requestRedraw();
    void setSelectionCallback(std::function<void(QString, QList<QPair<QString, QString>>)> callback);
    void setLockEdgesCallback(std::function<void(std::vector<EdgeId>)> callback);
    void setUnlockEdgesCallback(std::function<void(std::vector<EdgeId>)> callback);
    void setCandidateFaceCallback(std::function<void(FaceId)> callback);
    std::vector<FaceId> selectedFaceIds() const;
    std::vector<EdgeId> selectedEdgeIds() const;

protected:
    bool event(QEvent* event) override;
    QPaintEngine* paintEngine() const override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    QString selectionModeText() const;
    QPoint nativePosition(const QPointF& position) const;
    QPoint nativeDelta(const QPoint& delta) const;
    TopoDS_Shape detectedShapeAt(const QPointF& position);
    void updateHoverShape(const QPointF& position);
    void clearHoverShape();
    void clearFeatureEdgeShape();
    void clearLockedEdgeShape();
    void clearPatchOverlayShape();
    void clearPatchCutoutPreview();
    void selectAt(const QPointF& position, Qt::KeyboardModifiers modifiers);
    void redrawSelectedShapes();
    void applyCustomAspects();
    void initializeOcct();
    void activateSelectionMode();
    void resizeOcctWindow();
    void redrawView();

    bool hasDocument_ = false;
    ShapeStats stats_;
    const TopologyGraph* topology_ = nullptr;
    Handle(Aspect_DisplayConnection) displayConnection_;
    Handle(V3d_Viewer) viewer_;
    Handle(V3d_View) view_;
    Handle(WNT_Window) window_;
    Handle(AIS_InteractiveContext) context_;
    Handle(AIS_ColoredShape) displayedShape_;
    Handle(AIS_Shape) hoverShape_;
    Handle(AIS_Shape) featureEdgeShape_;
    Handle(AIS_Shape) lockedEdgeShape_;
    Handle(AIS_Triangulation) sourceStlShape_;
    Handle(AIS_Triangulation) croppedStlShape_;
    Handle(AIS_Shape) stlCropBoxShape_;
    Handle(AIS_Shape) patchCutoutPreviewShape_;
    Handle(AIS_Shape) patchOverlayShape_;
    std::vector<Handle(AIS_Shape)> mergeCandidateShapes_;
    std::vector<std::pair<FaceId, Quantity_Color>> mergeCandidateFaceColors_;
    SelectionMode selectionMode_ = SelectionMode::Face;
    std::function<void(QString, QList<QPair<QString, QString>>)> selectionCallback_;
    std::function<void(std::vector<EdgeId>)> lockEdgesCallback_;
    std::function<void(std::vector<EdgeId>)> unlockEdgesCallback_;
    std::function<void(FaceId)> candidateFaceCallback_;
    QPoint lastMousePosition_;
    bool rotating_ = false;
    bool panning_ = false;
    bool featureLinesVisible_ = true;
    bool mergePreviewVisible_ = false;
    bool sourceStlVisible_ = true;
    bool croppedStlVisible_ = true;
    bool stlCropBoxVisible_ = true;
    std::size_t sourceStlDisplayedTriangleCount_ = 0;
    std::size_t croppedStlDisplayedTriangleCount_ = 0;
    std::set<FaceId> selectedFaces_;
    std::set<EdgeId> selectedEdges_;
    int lastSelectedFace_ = -1;
    int lastSelectedEdge_ = -1;
};

}
