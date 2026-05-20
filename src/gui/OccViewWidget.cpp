#include "gui/OccViewWidget.h"

#include <AIS_SelectionScheme.hxx>
#include <Aspect_Handle.hxx>
#include <Aspect_TypeOfLine.hxx>
#include <Graphic3d_GraphicDriver.hxx>
#include <Graphic3d_NameOfMaterial.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Prs3d_LineAspect.hxx>
#include <Quantity_Color.hxx>
#include <SelectMgr_EntityOwner.hxx>
#include <StdSelect_BRepOwner.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopoDS_Shape.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>

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
#include <vector>

namespace spo {

namespace {

TopoDS_Shape shapeFromOwner(const Handle(SelectMgr_EntityOwner)& owner) {
    const auto brepOwner = Handle(StdSelect_BRepOwner)::DownCast(owner);
    if (brepOwner.IsNull()) {
        return {};
    }
    return brepOwner->Shape();
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

QPaintEngine* OccViewWidget::paintEngine() const {
    return context_.IsNull() ? QWidget::paintEngine() : nullptr;
}

void OccViewWidget::displayDocument(const ShapeDocument& document) {
    initializeOcct();
    hasDocument_ = document.hasShape();
    stats_ = document.stats();
    topology_ = &document.topology();
    selectedFace_ = -1;
    selectedEdge_ = -1;

    if (context_.IsNull()) {
        return;
    }

    context_->RemoveAll(Standard_False);
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
}

void OccViewWidget::clearDocument() {
    hasDocument_ = false;
    stats_ = {};
    topology_ = nullptr;
    selectedFace_ = -1;
    selectedEdge_ = -1;

    if (!context_.IsNull()) {
        context_->RemoveAll(Standard_False);
    }

    displayedShape_.Nullify();
    hoverShape_.Nullify();
    featureEdgeShape_.Nullify();
    redrawView();
}

void OccViewWidget::setSelectionMode(SelectionMode mode) {
    selectionMode_ = mode;
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
    if (!context_.IsNull() && !displayedShape_.IsNull()) {
        displayedShape_->SetTransparency(visible ? 0.25 : 0.0);
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
        menu.addAction("锁定为特征边");
        menu.addAction("解除特征边锁定");
        menu.addAction("显示边信息");
        menu.addAction("隐藏边");
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

void OccViewWidget::updateHoverShape(const QPointF& position) {
    if (!hasDocument_ || topology_ == nullptr || context_.IsNull() || view_.IsNull()) {
        return;
    }

    const auto shape = detectedShapeAt(position);
    const bool isExpectedShape =
        (selectionMode_ == SelectionMode::Face && topology_->faceIdFor(shape).has_value()) ||
        (selectionMode_ == SelectionMode::Edge && topology_->edgeIdFor(shape).has_value());

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

    Q_UNUSED(modifiers)

    const TopoDS_Shape selectedShape = detectedShapeAt(position);
    context_->ClearSelected(Standard_False);

    if (selectionMode_ == SelectionMode::Face) {
        selectedEdge_ = -1;
        const auto faceId = topology_->faceIdFor(selectedShape);
        if (!faceId.has_value()) {
            return;
        }

        clearHoverShape();
        displayedShape_->ClearCustomAspects();
        selectedFace_ = faceId.has_value() ? static_cast<int>(*faceId) : -1;
        displayedShape_->SetCustomColor(selectedShape, Quantity_Color(0.15, 0.80, 0.20, Quantity_TOC_RGB));
        displayedShape_->SetCustomTransparency(selectedShape, 0.0);
        if (selectionCallback_) {
            const auto faceEdges = faceId.has_value() ? topology_->edgesForFace(*faceId) : std::vector<EdgeId> {};
            const auto adjacentFaces = faceId.has_value() ? topology_->adjacentFaces(*faceId) : std::vector<FaceId> {};
            selectionCallback_("面", {
                {"选择模式", selectionModeText()},
                {"Face ID", faceId.has_value() ? QString::number(*faceId) : "未命中"},
                {"关联边数量", QString::number(faceEdges.size())},
                {"相邻面数量", QString::number(adjacentFaces.size())}
            });
        }
    } else if (selectionMode_ == SelectionMode::Edge) {
        selectedFace_ = -1;
        const auto edgeId = topology_->edgeIdFor(selectedShape);
        if (!edgeId.has_value()) {
            return;
        }

        clearHoverShape();
        displayedShape_->ClearCustomAspects();
        selectedEdge_ = edgeId.has_value() ? static_cast<int>(*edgeId) : -1;
        displayedShape_->SetCustomColor(selectedShape, Quantity_Color(0.15, 0.80, 0.20, Quantity_TOC_RGB));
        displayedShape_->SetCustomWidth(selectedShape, 3.0);
        if (selectionCallback_) {
            const auto* adjacency = edgeId.has_value() ? topology_->adjacencyForEdge(*edgeId) : nullptr;
            selectionCallback_("边", {
                {"选择模式", selectionModeText()},
                {"Edge ID", edgeId.has_value() ? QString::number(*edgeId) : "未命中"},
                {"邻接面数量", adjacency != nullptr ? QString::number(adjacency->faces.size()) : "0"},
                {"边界类型", adjacency != nullptr && adjacency->faces.size() == 1 ? "自由边" : "内部边"}
            });
        }
    } else if (selectionCallback_) {
        selectionCallback_("合并候选区域", {
            {"候选区域 ID", "待接入合并规划器"},
            {"面数量", "待接入合并规划器"},
            {"是否可应用", "待验证"}
        });
    }

    context_->Redisplay(displayedShape_, Standard_False);
    context_->UpdateCurrentViewer();
    redrawView();
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
