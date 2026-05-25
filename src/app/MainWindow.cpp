#include "app/MainWindow.h"

#include "gui/InspectPanel.h"
#include "gui/LogPanel.h"
#include "gui/ModelTreePanel.h"
#include "gui/OccViewWidget.h"
#include "gui/ParameterPanel.h"
#include "merge/FaceInspector.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QDockWidget>
#include <QFileDialog>
#include <QInputDialog>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

#include <algorithm>

namespace spo {

namespace {

QString candidateTypeText(MergeCandidateType type) {
    switch (type) {
    case MergeCandidateType::SameDomain:
        return "SameDomain";
    case MergeCandidateType::PlaneLike:
        return "PlaneLike";
    case MergeCandidateType::CylinderLike:
        return "CylinderLike";
    case MergeCandidateType::ConeLike:
        return "ConeLike";
    case MergeCandidateType::SphereLike:
        return "SphereLike";
    case MergeCandidateType::TorusLike:
        return "TorusLike";
    case MergeCandidateType::FreeformG1:
        return "FreeformG1";
    case MergeCandidateType::FreeformG2:
        return "FreeformG2";
    case MergeCandidateType::Unknown:
        return "Unknown";
    }
    return "Unknown";
}

QString riskLevelText(MergeRiskLevel risk) {
    switch (risk) {
    case MergeRiskLevel::Low:
        return "Low";
    case MergeRiskLevel::Medium:
        return "Medium";
    case MergeRiskLevel::High:
        return "High";
    }
    return "Unknown";
}

QString candidateStatusText(MergeCandidateStatus status) {
    return QString::fromLatin1(toString(status));
}

QString faceInspectCandidateStateText(FaceInspectCandidateState state) {
    return QString::fromLatin1(toString(state));
}

QString regionMergeFailureText(RegionMergeFailureReason reason) {
    switch (reason) {
    case RegionMergeFailureReason::None:
        return "None";
    case RegionMergeFailureReason::NotImplemented:
        return "NotImplemented";
    case RegionMergeFailureReason::NotSupported:
        return "NotSupported";
    case RegionMergeFailureReason::CandidateNotFound:
        return "CandidateNotFound";
    case RegionMergeFailureReason::InvalidCandidate:
        return "InvalidCandidate";
    case RegionMergeFailureReason::UnsupportedCandidateType:
        return "UnsupportedCandidateType";
    case RegionMergeFailureReason::RejectedCandidate:
        return "RejectedCandidate";
    case RegionMergeFailureReason::HiddenCandidate:
        return "HiddenCandidate";
    case RegionMergeFailureReason::InsufficientFaces:
        return "InsufficientFaces";
    case RegionMergeFailureReason::ProtectedEdgeConflict:
        return "ProtectedEdgeConflict";
    case RegionMergeFailureReason::LockedEdgeConflict:
        return "LockedEdgeConflict";
    case RegionMergeFailureReason::BoundaryLoopInvalid:
        return "BoundaryLoopInvalid";
    case RegionMergeFailureReason::MultipleOuterLoopsNotSupported:
        return "MultipleOuterLoopsNotSupported";
    case RegionMergeFailureReason::InnerLoopsNotSupported:
        return "InnerLoopsNotSupported";
    case RegionMergeFailureReason::PrimitiveFitFailed:
        return "PrimitiveFitFailed";
    case RegionMergeFailureReason::DeviationTooLarge:
        return "DeviationTooLarge";
    case RegionMergeFailureReason::SurfaceConstructionFailed:
        return "SurfaceConstructionFailed";
    case RegionMergeFailureReason::TopologyReplacementFailed:
        return "TopologyReplacementFailed";
    case RegionMergeFailureReason::SewingFailed:
        return "SewingFailed";
    case RegionMergeFailureReason::ValidationFailed:
        return "ValidationFailed";
    }
    return "Unknown";
}

struct CandidateStatusCounts {
    int pending = 0;
    int accepted = 0;
    int rejected = 0;
    int hidden = 0;
};

CandidateStatusCounts countCandidateStatuses(const std::vector<MergeCandidate>& candidates) {
    CandidateStatusCounts counts;
    for (const auto& candidate : candidates) {
        switch (candidate.status) {
        case MergeCandidateStatus::Pending:
            ++counts.pending;
            break;
        case MergeCandidateStatus::Accepted:
            ++counts.accepted;
            break;
        case MergeCandidateStatus::Rejected:
            ++counts.rejected;
            break;
        case MergeCandidateStatus::Hidden:
            ++counts.hidden;
            break;
        }
    }
    return counts;
}

void addVisibleCandidateId(std::set<int>& visibleIds, const MergeCandidate& candidate) {
    visibleIds.insert(candidate.candidate_id);
}

}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("STEP 曲面片优化器");
    menuBar()->setNativeMenuBar(false);
    resize(1380, 860);

    createActions();
    createMenus();
    createToolBars();
    createDocks();
    connectActions();

    qApp->installEventFilter(this);

    setStatus("就绪");
    logPanel_->appendInfo("界面初始化完成。");
}

bool MainWindow::event(QEvent* event) {
    if (event->type() == QEvent::KeyPress ||
        event->type() == QEvent::KeyRelease ||
        event->type() == QEvent::ShortcutOverride) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Alt) {
            keyEvent->accept();
            if (viewer_ != nullptr) {
                viewer_->requestRedraw();
            }
            return true;
        }
    }
    return QMainWindow::event(event);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    Q_UNUSED(watched)

    if (event->type() == QEvent::KeyPress ||
        event->type() == QEvent::KeyRelease ||
        event->type() == QEvent::ShortcutOverride) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Alt) {
            keyEvent->accept();
            if (viewer_ != nullptr) {
                viewer_->requestRedraw();
            }
            return true;
        }
    }

    if (event->type() == QEvent::ApplicationActivate ||
        event->type() == QEvent::ApplicationDeactivate ||
        event->type() == QEvent::FocusIn ||
        event->type() == QEvent::FocusOut ||
        event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseButtonRelease) {
        if (viewer_ != nullptr) {
            viewer_->requestRedraw();
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::createActions() {
    openStepAction_ = new QAction("打开 STEP/STP", this);
    openStepAction_->setShortcut(QKeySequence::Open);

    saveProjectAction_ = new QAction("保存项目", this);
    saveProjectAction_->setShortcut(QKeySequence::Save);

    exportStepAction_ = new QAction("导出 STEP", this);
    exportStepAction_->setShortcut(QKeySequence("Ctrl+E"));

    exitAction_ = new QAction("退出", this);
    exitAction_->setShortcut(QKeySequence::Quit);

    selectFaceAction_ = new QAction("选择面", this);
    selectFaceAction_->setCheckable(true);
    selectFaceAction_->setChecked(true);
    selectFaceAction_->setShortcut(QKeySequence("F"));

    selectEdgeAction_ = new QAction("选择边", this);
    selectEdgeAction_->setCheckable(true);
    selectEdgeAction_->setShortcut(QKeySequence("E"));

    selectCandidateAction_ = new QAction("选择候选区域", this);
    selectCandidateAction_->setCheckable(true);

    auto* selectionGroup = new QActionGroup(this);
    selectionGroup->setExclusive(true);
    selectionGroup->addAction(selectFaceAction_);
    selectionGroup->addAction(selectEdgeAction_);
    selectionGroup->addAction(selectCandidateAction_);

    detectAction_ = new QAction("检测特征边", this);
    previewMergeAction_ = new QAction("预览合并", this);
    previewMergeAction_->setShortcut(QKeySequence("M"));
    showAllMergeCandidatesAction_ = new QAction("显示全部非隐藏候选", this);
    highlightMergeCandidateByIdAction_ = new QAction("按 ID 高亮候选", this);
    clearMergeCandidatesAction_ = new QAction("清除候选高亮", this);
    acceptMergeCandidateAction_ = new QAction("接受当前候选", this);
    rejectMergeCandidateAction_ = new QAction("拒绝当前候选", this);
    hideMergeCandidateAction_ = new QAction("隐藏当前候选", this);
    restoreMergeCandidateAction_ = new QAction("恢复当前候选", this);
    showAcceptedMergeCandidatesAction_ = new QAction("显示已接受候选", this);
    showPendingMergeCandidatesAction_ = new QAction("显示待处理候选", this);
    mergePlaneCandidateAction_ = new QAction("合并当前平面候选", this);
    mergeAcceptedPlaneCandidatesAction_ = new QAction("合并所有已接受平面候选", this);
    mergeAllPlaneCandidatesAction_ = new QAction("一键合并全部可合并平面候选", this);
    applyMergeAction_ = new QAction("执行合并", this);

    validateAction_ = new QAction("合法性检查", this);
    validateAction_->setShortcut(QKeySequence("V"));

    resetViewAction_ = new QAction("重置视角", this);
    resetViewAction_->setShortcut(QKeySequence("R"));

    fitAllAction_ = new QAction("适应窗口", this);

    toggleFeaturesAction_ = new QAction("显示特征线", this);
    toggleFeaturesAction_->setCheckable(true);
    toggleFeaturesAction_->setChecked(true);
    toggleFeaturesAction_->setShortcut(QKeySequence("H"));

    undoAction_ = new QAction("撤销", this);
    undoAction_->setShortcut(QKeySequence::Undo);
    undoAction_->setEnabled(false);

    redoAction_ = new QAction("重做", this);
    redoAction_->setShortcut(QKeySequence::Redo);
    redoAction_->setEnabled(false);
}

void MainWindow::createMenus() {
    auto* fileMenu = menuBar()->addMenu("文件");
    fileMenu->addAction(openStepAction_);
    fileMenu->addAction(saveProjectAction_);
    fileMenu->addSeparator();
    fileMenu->addAction(exportStepAction_);
    fileMenu->addSeparator();
    fileMenu->addAction(exitAction_);

    viewMenu_ = menuBar()->addMenu("视图");
    viewMenu_->addAction(selectFaceAction_);
    viewMenu_->addAction(selectEdgeAction_);
    viewMenu_->addAction(selectCandidateAction_);
    viewMenu_->addSeparator();
    viewMenu_->addAction(toggleFeaturesAction_);
    viewMenu_->addAction(resetViewAction_);
    viewMenu_->addAction(fitAllAction_);

    auto* detectMenu = menuBar()->addMenu("检测");
    detectMenu->addAction(detectAction_);

    auto* mergeMenu = menuBar()->addMenu("合并");
    mergeMenu->addAction(previewMergeAction_);
    mergeMenu->addAction(showAllMergeCandidatesAction_);
    mergeMenu->addAction(highlightMergeCandidateByIdAction_);
    mergeMenu->addAction(clearMergeCandidatesAction_);
    mergeMenu->addSeparator();
    mergeMenu->addAction(acceptMergeCandidateAction_);
    mergeMenu->addAction(rejectMergeCandidateAction_);
    mergeMenu->addAction(hideMergeCandidateAction_);
    mergeMenu->addAction(restoreMergeCandidateAction_);
    mergeMenu->addSeparator();
    mergeMenu->addAction(showAcceptedMergeCandidatesAction_);
    mergeMenu->addAction(showPendingMergeCandidatesAction_);
    mergeMenu->addSeparator();
    planeMergeMenu_ = mergeMenu->addMenu("平面候选合并");
    planeMergeMenu_->addAction(mergePlaneCandidateAction_);
    planeMergeMenu_->addAction(mergeAcceptedPlaneCandidatesAction_);
    planeMergeMenu_->addAction(mergeAllPlaneCandidatesAction_);
    mergeMenu->addSeparator();
    mergeMenu->addAction(applyMergeAction_);
    mergeMenu->addSeparator();
    mergeMenu->addAction(undoAction_);
    mergeMenu->addAction(redoAction_);

    auto* validateMenu = menuBar()->addMenu("验证");
    validateMenu->addAction(validateAction_);

    auto* exportMenu = menuBar()->addMenu("导出");
    exportMenu->addAction(exportStepAction_);

    auto* helpMenu = menuBar()->addMenu("帮助");
    helpMenu->addAction("关于", this, [this]() {
        QMessageBox::about(this, "关于", "STEP 曲面片优化器\n特征感知的 STEP 曲面片合并与边界优化系统");
    });
}

void MainWindow::createToolBars() {
    auto* toolBar = addToolBar("主工具栏");
    toolBar->setMovable(false);
    toolBar->addAction(openStepAction_);
    toolBar->addAction(saveProjectAction_);
    toolBar->addSeparator();
    toolBar->addAction(selectFaceAction_);
    toolBar->addAction(selectEdgeAction_);
    toolBar->addAction(selectCandidateAction_);
    toolBar->addSeparator();
    toolBar->addAction(detectAction_);
    toolBar->addAction(previewMergeAction_);
    toolBar->addAction(showAllMergeCandidatesAction_);
    toolBar->addAction(clearMergeCandidatesAction_);
    toolBar->addAction(acceptMergeCandidateAction_);
    toolBar->addAction(rejectMergeCandidateAction_);
    toolBar->addAction(hideMergeCandidateAction_);
    auto* planeMergeButton = new QToolButton(this);
    planeMergeButton->setText("平面合并");
    planeMergeButton->setPopupMode(QToolButton::InstantPopup);
    auto* planeMergeToolMenu = new QMenu(planeMergeButton);
    planeMergeToolMenu->addAction(mergePlaneCandidateAction_);
    planeMergeToolMenu->addAction(mergeAcceptedPlaneCandidatesAction_);
    planeMergeToolMenu->addAction(mergeAllPlaneCandidatesAction_);
    planeMergeButton->setMenu(planeMergeToolMenu);
    toolBar->addWidget(planeMergeButton);
    toolBar->addAction(applyMergeAction_);
    toolBar->addAction(validateAction_);
    toolBar->addAction(exportStepAction_);
    toolBar->addSeparator();
    toolBar->addAction(undoAction_);
    toolBar->addAction(redoAction_);
}

void MainWindow::createDocks() {
    viewer_ = new OccViewWidget(this);
    setCentralWidget(viewer_);

    modelTree_ = new ModelTreePanel(this);
    modelDock_ = new QDockWidget("模型树", this);
    modelDock_->setObjectName("modelTreeDock");
    modelDock_->setWidget(modelTree_);
    addDockWidget(Qt::LeftDockWidgetArea, modelDock_);

    parameterPanel_ = new ParameterPanel(this);
    parameterDock_ = new QDockWidget("参数", this);
    parameterDock_->setObjectName("parameterDock");
    parameterDock_->setWidget(parameterPanel_);
    addDockWidget(Qt::RightDockWidgetArea, parameterDock_);

    inspectPanel_ = new InspectPanel(this);
    logPanel_ = new LogPanel(this);

    bottomTabs_ = new QTabWidget(this);
    bottomTabs_->addTab(logPanel_, "日志");
    bottomTabs_->addTab(inspectPanel_, "检查");
    bottomTabs_->addTab(inspectPanel_->validationWidget(), "验证");
    bottomTabs_->addTab(inspectPanel_->reportWidget(), "报告");

    bottomDock_ = new QDockWidget("输出", this);
    bottomDock_->setObjectName("outputDock");
    bottomDock_->setWidget(bottomTabs_);
    addDockWidget(Qt::BottomDockWidgetArea, bottomDock_);

    if (viewMenu_ != nullptr) {
        viewMenu_->addSeparator();
        modelDock_->toggleViewAction()->setText("显示模型树");
        parameterDock_->toggleViewAction()->setText("显示参数面板");
        bottomDock_->toggleViewAction()->setText("显示输出面板");
        viewMenu_->addAction(modelDock_->toggleViewAction());
        viewMenu_->addAction(parameterDock_->toggleViewAction());
        viewMenu_->addAction(bottomDock_->toggleViewAction());
    }

    viewer_->setSelectionCallback([this](const QString& title, const QList<QPair<QString, QString>>& rows) {
        inspectPanel_->showProperties(title, rows);
        bottomTabs_->setCurrentWidget(inspectPanel_);
    });
    viewer_->setLockEdgesCallback([this](std::vector<EdgeId> edgeIds) {
        lockSelectedEdges(edgeIds);
    });
    viewer_->setUnlockEdgesCallback([this](std::vector<EdgeId> edgeIds) {
        unlockSelectedEdges(edgeIds);
    });
    viewer_->setCandidateFaceCallback([this](FaceId faceId) {
        selectMergeCandidateByFace(faceId);
    });
}

void MainWindow::connectActions() {
    connect(openStepAction_, &QAction::triggered, this, [this]() { openStepFile(); });
    connect(saveProjectAction_, &QAction::triggered, this, [this]() { saveProject(); });
    connect(exportStepAction_, &QAction::triggered, this, [this]() { exportStepFile(); });
    connect(exitAction_, &QAction::triggered, this, &QWidget::close);

    connect(selectFaceAction_, &QAction::triggered, this, [this]() {
        viewer_->setSelectionMode(SelectionMode::Face);
        setStatus("选择面模式");
    });
    connect(selectEdgeAction_, &QAction::triggered, this, [this]() {
        viewer_->setSelectionMode(SelectionMode::Edge);
        setStatus("选择边模式");
    });
    connect(selectCandidateAction_, &QAction::triggered, this, [this]() {
        viewer_->setSelectionMode(SelectionMode::Candidate);
        setStatus("候选区域选择模式");
    });

    connect(detectAction_, &QAction::triggered, this, [this]() { detectFeatureEdges(); });
    connect(previewMergeAction_, &QAction::triggered, this, [this]() { previewMergeCandidates(); });
    connect(showAllMergeCandidatesAction_, &QAction::triggered, this, [this]() { showAllMergeCandidates(); });
    connect(highlightMergeCandidateByIdAction_, &QAction::triggered, this, [this]() { highlightMergeCandidateById(); });
    connect(clearMergeCandidatesAction_, &QAction::triggered, this, [this]() { clearMergeCandidatePreview(); });
    connect(acceptMergeCandidateAction_, &QAction::triggered, this, [this]() { acceptCurrentMergeCandidate(); });
    connect(rejectMergeCandidateAction_, &QAction::triggered, this, [this]() { rejectCurrentMergeCandidate(); });
    connect(hideMergeCandidateAction_, &QAction::triggered, this, [this]() { hideCurrentMergeCandidate(); });
    connect(restoreMergeCandidateAction_, &QAction::triggered, this, [this]() { restoreCurrentMergeCandidate(); });
    connect(showAcceptedMergeCandidatesAction_, &QAction::triggered, this, [this]() { showAcceptedMergeCandidates(); });
    connect(showPendingMergeCandidatesAction_, &QAction::triggered, this, [this]() { showPendingMergeCandidates(); });
    connect(mergePlaneCandidateAction_, &QAction::triggered, this, [this]() { mergeCurrentPlaneCandidate(); });
    connect(mergeAcceptedPlaneCandidatesAction_, &QAction::triggered, this, [this]() { mergeAcceptedPlaneCandidates(); });
    connect(mergeAllPlaneCandidatesAction_, &QAction::triggered, this, [this]() { mergeAllMergeablePlaneCandidates(); });
    connect(applyMergeAction_, &QAction::triggered, this, [this]() { applyMerge(); });
    connect(validateAction_, &QAction::triggered, this, [this]() { validateShape(); });
    connect(resetViewAction_, &QAction::triggered, this, [this]() { resetView(); });
    connect(fitAllAction_, &QAction::triggered, viewer_, &OccViewWidget::fitAll);
    connect(toggleFeaturesAction_, &QAction::toggled, viewer_, &OccViewWidget::setFeatureLinesVisible);
    connect(undoAction_, &QAction::triggered, this, [this]() { undo(); });
    connect(redoAction_, &QAction::triggered, this, [this]() { redo(); });
}

void MainWindow::openStepFile() {
    const auto filePath = QFileDialog::getOpenFileName(
        this, "打开 STEP/STP", QString(), "STEP 文件 (*.step *.stp *.STEP *.STP);;所有文件 (*.*)");
    if (filePath.isEmpty()) {
        return;
    }

    const auto result = controller_.openStepFile(filePath.toStdString());
    if (!result.success()) {
        QMessageBox::critical(this, "打开 STEP/STP 失败", QString::fromStdString(result.message()));
        logPanel_->appendError(QString("打开失败：%1").arg(QString::fromStdString(result.message())));
        return;
    }

    const auto& document = controller_.document();
    const auto& stats = document.stats();
    clearMergeCandidateState();
    hasFeatureEdgeResult_ = false;
    refreshModelTree();
    viewer_->displayDocument(document);
    syncLockedEdges();
    QTimer::singleShot(0, viewer_, &OccViewWidget::fitAll);
    QTimer::singleShot(100, viewer_, &OccViewWidget::fitAll);
    inspectPanel_->showProperties("模型摘要", {
        {"文件", QString::fromStdString(document.sourcePath().string())},
        {"实体数", QString::number(stats.solids)},
        {"壳数", QString::number(stats.shells)},
        {"面数", QString::number(stats.faces)},
        {"边数", QString::number(stats.edges)},
        {"顶点数", QString::number(stats.vertices)}
    });
    inspectPanel_->showReport(QString("输入文件：%1\n实体数：%2\n壳数：%3\n面数：%4\n边数：%5")
        .arg(QString::fromStdString(document.sourcePath().string()))
        .arg(stats.solids)
        .arg(stats.shells)
        .arg(stats.faces)
        .arg(stats.edges));
    logPanel_->appendInfo(QString("已打开 STEP/STP：%1").arg(filePath));
    setStatus("STEP/STP 已加载");
    refreshUndoRedoActions();
}

void MainWindow::saveProject() {
    logPanel_->appendWarning("项目保存功能尚未实现。");
    setStatus("项目保存待实现");
}

void MainWindow::exportStepFile() {
    if (!controller_.hasDocument()) {
        QMessageBox::information(this, "导出 STEP", "请先打开 STEP/STP 文件再导出。");
        return;
    }

    const auto filePath = QFileDialog::getSaveFileName(
        this, "导出 STEP", QString(), "STEP 文件 (*.step *.stp);;所有文件 (*.*)");
    if (filePath.isEmpty()) {
        return;
    }

    const auto result = controller_.exportStepFile(filePath.toStdString());
    if (!result.success()) {
        QMessageBox::critical(this, "导出 STEP 失败", QString::fromStdString(result.message()));
        logPanel_->appendError(QString("导出失败：%1").arg(QString::fromStdString(result.message())));
        return;
    }

    const auto verifyResult = controller_.verifyStepFileReadable(filePath.toStdString());
    if (!verifyResult.success()) {
        logPanel_->appendError(QString("STEP 已导出，但重新读取校验失败：%1").arg(QString::fromStdString(verifyResult.message())));
        inspectPanel_->showValidation(QString("导出后二次读取校验失败\n文件：%1\n错误：%2")
            .arg(filePath)
            .arg(QString::fromStdString(verifyResult.message())));
        setStatus("STEP 已导出，二次读取失败");
        refreshUndoRedoActions();
        return;
    }

    logPanel_->appendInfo(QString("已导出 STEP 并通过二次读取校验：%1").arg(filePath));
    inspectPanel_->showValidation(QString("导出后二次读取校验通过\n文件：%1").arg(filePath));
    setStatus("STEP 已导出并通过校验");
    refreshUndoRedoActions();
}

void MainWindow::detectFeatureEdges() {
    if (!controller_.hasDocument()) {
        inspectPanel_->showReport("请先打开 STEP/STP 文件。");
        setStatus("未加载模型");
        return;
    }

    const auto params = parameterPanel_->parameters();
    const auto result = controller_.detectFeatureEdges(params.angular_threshold_degrees, params.min_edge_length);
    hasFeatureEdgeResult_ = true;
    viewer_->showFeatureEdges(result);
    viewer_->setFeatureLinesVisible(true);
    toggleFeaturesAction_->setChecked(true);
    refreshModelTree();
    inspectPanel_->showReport(QString("特征边检测完成\n角度阈值：%1 度\n特征边总数：%2\nSharp edge：%3\nFree edge：%4\nMultiple edge：%5")
        .arg(params.angular_threshold_degrees)
        .arg(result.edges.size())
        .arg(result.sharp_edges)
        .arg(result.free_edges)
        .arg(result.multiple_edges));
    logPanel_->appendInfo(QString("特征边检测完成：%1 条").arg(result.edges.size()));
    setStatus("特征边检测完成");
    refreshUndoRedoActions();
}

void MainWindow::previewMergeCandidates() {
    if (!controller_.hasDocument()) {
        inspectPanel_->showReport("请先打开 STEP/STP 文件。");
        setStatus("未加载模型");
        return;
    }

    const auto beforeStats = controller_.document().stats();
    const auto params = parameterPanel_->parameters();
    MergePlannerOptions options;
    options.max_plane_distance = params.linear_tolerance;
    options.min_region_faces = 2;

    const auto result = controller_.previewMergeCandidates(
        params.angular_threshold_degrees,
        params.min_edge_length,
        options);
    const auto afterStats = controller_.document().stats();
    clearMergeCandidateState();
    lastMergeCandidates_ = result.candidates;
    hasFeatureEdgeResult_ = true;

    int maxFaceCount = 0;
    int totalCandidateFaces = 0;
    for (const auto& candidate : result.candidates) {
        maxFaceCount = std::max(maxFaceCount, candidate.face_count);
        totalCandidateFaces += candidate.face_count;
    }
    const auto statusCounts = countCandidateStatuses(lastMergeCandidates_);

    QString report = QString("合并候选区域预览完成\n候选区域数量：%1\nPending：%2\nAccepted：%3\nRejected：%4\nHidden：%5\n保护边数量：%6\n访问 face 数量：%7\n拒绝区域数量：%8\n最大候选区域 face 数：%9\n总候选 face 数：%10\n预览前 face/edge：%11/%12\n预览后 face/edge：%13/%14")
        .arg(result.candidates.size())
        .arg(statusCounts.pending)
        .arg(statusCounts.accepted)
        .arg(statusCounts.rejected)
        .arg(statusCounts.hidden)
        .arg(result.protected_edge_count)
        .arg(result.visited_faces)
        .arg(result.rejected_regions)
        .arg(maxFaceCount)
        .arg(totalCandidateFaces)
        .arg(beforeStats.faces)
        .arg(beforeStats.edges)
        .arg(afterStats.faces)
        .arg(afterStats.edges);

    const auto previewCount = std::min<std::size_t>(10, result.candidates.size());
    for (std::size_t index = 0; index < previewCount; ++index) {
        const auto& candidate = result.candidates[index];
        report += QString("\n\n候选 %1\n状态：%2\n类型：%3\nface 数：%4\n总面积：%5\n最大法向夹角：%6 度\n最大距离：%7\n边界边数：%8\n内部边数：%9\n风险：%10")
            .arg(candidate.candidate_id)
            .arg(candidateStatusText(candidate.status))
            .arg(candidateTypeText(candidate.candidate_type))
            .arg(candidate.face_count)
            .arg(QString::number(candidate.total_area, 'f', 4))
            .arg(QString::number(candidate.max_normal_angle_deg, 'f', 3))
            .arg(QString::number(candidate.max_distance, 'g', 6))
            .arg(candidate.boundary_edge_count)
            .arg(candidate.internal_edge_count)
            .arg(riskLevelText(candidate.risk_level));
    }

    if (result.candidates.empty()) {
        viewer_->clearMergeCandidates();
        visibleMergeCandidateCount_ = 0;
        visibleMergeCandidateIds_.clear();
        report += "\n\n没有可预览候选区域。";
    } else {
        viewer_->showMergeCandidates(result.candidates, 10, false);
        visibleMergeCandidateCount_ = static_cast<int>(std::min<std::size_t>(10, result.candidates.size()));
        visibleMergeCandidateIds_.clear();
        for (std::size_t index = 0; index < static_cast<std::size_t>(visibleMergeCandidateCount_); ++index) {
            addVisibleCandidateId(visibleMergeCandidateIds_, result.candidates[index]);
        }
    }
    viewer_->showFeatureEdges(controller_.featureEdges());
    viewer_->setFeatureLinesVisible(true);
    toggleFeaturesAction_->setChecked(true);
    refreshModelTree();
    inspectPanel_->showReport(report);
    logPanel_->appendInfo(QString("合并候选区域预览完成：候选 %1 个，保护边 %2，访问 face %3，拒绝 %4")
        .arg(result.candidates.size())
        .arg(result.protected_edge_count)
        .arg(result.visited_faces)
        .arg(result.rejected_regions));
    setStatus("合并候选区域预览完成");
}

void MainWindow::showAllMergeCandidates() {
    if (lastMergeCandidates_.empty()) {
        logPanel_->appendWarning("当前没有候选区域，请先点击“预览合并”。");
        setStatus("没有候选区域");
        return;
    }

    showNonHiddenMergeCandidates();
}

void MainWindow::showNonHiddenMergeCandidates() {
    if (lastMergeCandidates_.empty()) {
        logPanel_->appendWarning("当前没有候选区域，请先点击“预览合并”。");
        setStatus("没有候选区域");
        return;
    }

    std::vector<MergeCandidate> visibleCandidates;
    for (const auto& candidate : lastMergeCandidates_) {
        if (candidate.status != MergeCandidateStatus::Hidden) {
            visibleCandidates.push_back(candidate);
        }
    }

    viewer_->showMergeCandidates(visibleCandidates, 10, true);
    if (hasFeatureEdgeResult_) {
        viewer_->showFeatureEdges(controller_.featureEdges());
    }
    visibleMergeCandidateCount_ = static_cast<int>(visibleCandidates.size());
    visibleMergeCandidateIds_.clear();
    for (const auto& candidate : visibleCandidates) {
        addVisibleCandidateId(visibleMergeCandidateIds_, candidate);
    }
    refreshModelTree();
    showCandidateStatusReport("已显示全部非隐藏候选区域");
    setStatus("已显示全部非隐藏候选区域");
}

void MainWindow::highlightMergeCandidateById() {
    if (lastMergeCandidates_.empty()) {
        logPanel_->appendWarning("当前没有候选区域，请先点击“预览合并”。");
        setStatus("没有候选区域");
        return;
    }

    bool ok = false;
    const auto candidateId = QInputDialog::getInt(
        this,
        "按 ID 高亮候选",
        "候选 ID：",
        0,
        0,
        1000000,
        1,
        &ok);
    if (!ok) {
        return;
    }

    auto it = std::find_if(lastMergeCandidates_.begin(), lastMergeCandidates_.end(), [candidateId](const auto& candidate) {
        return candidate.candidate_id == candidateId;
    });
    if (it == lastMergeCandidates_.end()) {
        visibleMergeCandidateCount_ = 0;
        visibleMergeCandidateIds_.clear();
        refreshModelTree();
        logPanel_->appendWarning(QString("未找到候选 ID：%1").arg(candidateId));
        setStatus("未找到候选区域");
        return;
    }

    currentMergeCandidateId_ = candidateId;
    if (it->status == MergeCandidateStatus::Hidden) {
        viewer_->clearMergeCandidates();
        visibleMergeCandidateCount_ = 0;
        visibleMergeCandidateIds_.clear();
        refreshModelTree();
        showCandidateStatusReport(QString("候选 %1 当前为 Hidden，未显示").arg(candidateId));
        setStatus("候选已隐藏");
        return;
    }

    viewer_->showMergeCandidateById(std::vector<MergeCandidate>{*it}, candidateId);
    if (hasFeatureEdgeResult_) {
        viewer_->showFeatureEdges(controller_.featureEdges());
    }
    visibleMergeCandidateCount_ = 1;
    visibleMergeCandidateIds_.clear();
    visibleMergeCandidateIds_.insert(candidateId);
    refreshModelTree();
    showCandidateStatusReport(QString("已高亮候选 ID：%1").arg(candidateId));
    setStatus(QString("已高亮候选 %1").arg(candidateId));
}

void MainWindow::selectMergeCandidateByFace(FaceId faceId) {
    if (!controller_.hasDocument()) {
        inspectPanel_->showReport("请先打开 STEP/STP 文件。");
        setStatus("未加载模型");
        return;
    }

    const auto info = inspectFace(
        controller_.document(),
        faceId,
        lastMergeCandidates_,
        visibleMergeCandidateIds_,
        hasFeatureEdgeResult_ ? &controller_.featureEdges() : nullptr,
        controller_.lockedEdges());

    MergeCandidate* matchedCandidate = nullptr;
    MergeCandidate* hiddenCandidate = nullptr;
    for (auto& candidate : lastMergeCandidates_) {
        if (std::find(candidate.faces.begin(), candidate.faces.end(), faceId) == candidate.faces.end()) {
            continue;
        }
        if (candidate.status == MergeCandidateStatus::Hidden) {
            if (hiddenCandidate == nullptr) {
                hiddenCandidate = &candidate;
            }
            continue;
        }
        matchedCandidate = &candidate;
        break;
    }

    if (matchedCandidate == nullptr) {
        visibleMergeCandidateCount_ = 0;
        visibleMergeCandidateIds_.clear();
        refreshModelTree();
        if (hiddenCandidate != nullptr) {
            currentMergeCandidateId_ = hiddenCandidate->candidate_id;
            viewer_->clearMergeCandidates();
            showFaceInspectReport(info, !lastMergeCandidates_.empty());
            logPanel_->appendInfo(QString("Face %1 属于隐藏候选 %2，未显示").arg(faceId).arg(currentMergeCandidateId_));
            setStatus("候选已隐藏");
        } else {
            showFaceInspectReport(info, !lastMergeCandidates_.empty());
            logPanel_->appendWarning(QString("Face %1 不属于当前候选区域。").arg(faceId));
            setStatus(lastMergeCandidates_.empty() ? "未生成候选区域" : "未命中候选区域");
        }
        return;
    }

    currentMergeCandidateId_ = matchedCandidate->candidate_id;
    viewer_->showMergeCandidateById(std::vector<MergeCandidate>{*matchedCandidate}, matchedCandidate->candidate_id);
    if (hasFeatureEdgeResult_) {
        viewer_->showFeatureEdges(controller_.featureEdges());
    }
    visibleMergeCandidateCount_ = 1;
    visibleMergeCandidateIds_.clear();
    visibleMergeCandidateIds_.insert(currentMergeCandidateId_);
    refreshModelTree();
    const auto selectedInfo = inspectFace(
        controller_.document(),
        faceId,
        lastMergeCandidates_,
        visibleMergeCandidateIds_,
        hasFeatureEdgeResult_ ? &controller_.featureEdges() : nullptr,
        controller_.lockedEdges());
    showFaceInspectReport(selectedInfo, true);
    logPanel_->appendInfo(QString("已选择候选 ID：%1").arg(currentMergeCandidateId_));
    setStatus(QString("已选择候选 %1").arg(currentMergeCandidateId_));
}

void MainWindow::clearMergeCandidatePreview() {
    viewer_->clearMergeCandidates();
    visibleMergeCandidateCount_ = 0;
    visibleMergeCandidateIds_.clear();
    refreshModelTree();
    logPanel_->appendInfo("已清除候选区域高亮。");
    setStatus("候选高亮已清除");
}

void MainWindow::acceptCurrentMergeCandidate() {
    if (setCurrentMergeCandidateStatus(MergeCandidateStatus::Accepted)) {
        showCandidateStatusReport(QString("已接受候选 ID：%1").arg(currentMergeCandidateId_));
        setStatus("候选已接受");
    }
}

void MainWindow::rejectCurrentMergeCandidate() {
    if (setCurrentMergeCandidateStatus(MergeCandidateStatus::Rejected)) {
        viewer_->clearMergeCandidates();
        visibleMergeCandidateCount_ = 0;
        refreshModelTree();
        showCandidateStatusReport(QString("已拒绝候选 ID：%1").arg(currentMergeCandidateId_));
        setStatus("候选已拒绝");
    }
}

void MainWindow::hideCurrentMergeCandidate() {
    if (setCurrentMergeCandidateStatus(MergeCandidateStatus::Hidden)) {
        viewer_->clearMergeCandidates();
        visibleMergeCandidateCount_ = 0;
        refreshModelTree();
        showCandidateStatusReport(QString("已隐藏候选 ID：%1").arg(currentMergeCandidateId_));
        setStatus("候选已隐藏");
    }
}

void MainWindow::restoreCurrentMergeCandidate() {
    if (setCurrentMergeCandidateStatus(MergeCandidateStatus::Pending)) {
        showCandidateStatusReport(QString("已恢复候选 ID：%1").arg(currentMergeCandidateId_));
        setStatus("候选已恢复为待处理");
    }
}

void MainWindow::showAcceptedMergeCandidates() {
    showFilteredMergeCandidates(MergeCandidateStatus::Accepted);
}

void MainWindow::showPendingMergeCandidates() {
    showFilteredMergeCandidates(MergeCandidateStatus::Pending);
}

void MainWindow::mergeCurrentPlaneCandidate() {
    if (!controller_.hasDocument()) {
        inspectPanel_->showReport("请先打开 STEP/STP 文件。");
        setStatus("未加载模型");
        return;
    }

    auto* candidate = currentMergeCandidate();
    if (candidate == nullptr) {
        inspectPanel_->showReport("请先在候选选择模式下点击一个候选区域，或按 ID 高亮一个候选区域。");
        logPanel_->appendWarning("执行平面候选合并前未选择候选区域。");
        setStatus("未选择候选区域");
        return;
    }

    if (candidate->candidate_type != MergeCandidateType::PlaneLike) {
        inspectPanel_->showReport(QString("当前候选不是 PlaneLike，不能执行平面区域合并。\n候选 ID：%1\n候选类型：%2")
            .arg(candidate->candidate_id)
            .arg(candidateTypeText(candidate->candidate_type)));
        setStatus("当前候选不是平面候选");
        return;
    }

    const auto params = parameterPanel_->parameters();
    PlaneRegionMergeOptions options;
    options.plane_distance_tolerance = std::max(options.plane_distance_tolerance, params.linear_tolerance);
    options.allow_pending_candidate = true;
    options.require_accepted_candidate = false;
    options.min_region_faces = 2;

    const auto result = controller_.mergePlaneCandidate(*candidate, options);
    const auto report = QString("平面候选合并%1\n候选 ID：%2\n候选类型：%3\n候选状态：%4\n失败原因：%5\n消息：%6\n平面法向：(%7, %8, %9)\n合并前 face：%10\n合并后 face：%11\nface reduction ratio：%12%\n合并前 edge：%13\n合并后 edge：%14\nedge reduction ratio：%15%\nmax deviation：%16\nmean deviation：%17\nrms deviation：%18\nBRepCheck：%19")
        .arg(result.success ? "完成" : "失败")
        .arg(candidate->candidate_id)
        .arg(candidateTypeText(candidate->candidate_type))
        .arg(candidateStatusText(candidate->status))
        .arg(regionMergeFailureText(result.failure_reason))
        .arg(QString::fromStdString(result.message))
        .arg(QString::number(result.plane_normal_x, 'f', 6))
        .arg(QString::number(result.plane_normal_y, 'f', 6))
        .arg(QString::number(result.plane_normal_z, 'f', 6))
        .arg(result.face_count_before)
        .arg(result.face_count_after)
        .arg(QString::number(result.face_reduction_ratio * 100.0, 'f', 2))
        .arg(result.edge_count_before)
        .arg(result.edge_count_after)
        .arg(QString::number(result.edge_reduction_ratio * 100.0, 'f', 2))
        .arg(QString::number(result.max_deviation, 'g', 6))
        .arg(QString::number(result.mean_deviation, 'g', 6))
        .arg(QString::number(result.rms_deviation, 'g', 6))
        .arg(result.brep_check_valid ? "通过" : "失败");

    inspectPanel_->showReport(report);
    if (!result.success) {
        logPanel_->appendWarning(QString("平面候选合并失败：候选 %1，原因 %2，%3")
            .arg(candidate->candidate_id)
            .arg(regionMergeFailureText(result.failure_reason))
            .arg(QString::fromStdString(result.message)));
        setStatus("平面候选合并失败");
        refreshUndoRedoActions();
        return;
    }

    refreshDocumentViews();
    logPanel_->appendInfo(QString("平面候选合并完成：候选 %1，face %2 -> %3，edge %4 -> %5")
        .arg(result.candidate_id)
        .arg(result.face_count_before)
        .arg(result.face_count_after)
        .arg(result.edge_count_before)
        .arg(result.edge_count_after));
    setStatus("平面候选合并完成");
    refreshUndoRedoActions();
}

void MainWindow::mergeAcceptedPlaneCandidates() {
    std::vector<MergeCandidate> candidates;
    for (const auto& candidate : lastMergeCandidates_) {
        if (candidate.candidate_type == MergeCandidateType::PlaneLike &&
            candidate.status == MergeCandidateStatus::Accepted) {
            candidates.push_back(candidate);
        }
    }
    mergePlaneCandidateBatch(candidates, "合并所有已接受平面候选");
}

void MainWindow::mergeAllMergeablePlaneCandidates() {
    std::vector<MergeCandidate> candidates;
    for (const auto& candidate : lastMergeCandidates_) {
        if (candidate.valid &&
            candidate.candidate_type == MergeCandidateType::PlaneLike &&
            candidate.status != MergeCandidateStatus::Rejected &&
            candidate.status != MergeCandidateStatus::Hidden) {
            candidates.push_back(candidate);
        }
    }
    mergePlaneCandidateBatch(candidates, "一键合并全部可合并平面候选");
}

void MainWindow::mergePlaneCandidateBatch(const std::vector<MergeCandidate>& candidates, const QString& title) {
    if (!controller_.hasDocument()) {
        inspectPanel_->showReport("请先打开 STEP/STP 文件。");
        setStatus("未加载模型");
        return;
    }
    if (lastMergeCandidates_.empty()) {
        inspectPanel_->showReport("请先点击“预览合并”生成候选区域。");
        logPanel_->appendWarning("批量平面合并前尚未生成候选区域。");
        setStatus("没有候选区域");
        return;
    }
    if (candidates.empty()) {
        inspectPanel_->showReport(QString("%1\n没有符合条件的 PlaneLike 候选区域。").arg(title));
        logPanel_->appendWarning(QString("%1：没有符合条件的 PlaneLike 候选区域。").arg(title));
        setStatus("没有可批量合并的平面候选");
        return;
    }

    PlaneRegionMergeOptions options;
    const auto params = parameterPanel_->parameters();
    options.plane_distance_tolerance = std::max(options.plane_distance_tolerance, params.linear_tolerance);
    options.allow_pending_candidate = true;
    options.require_accepted_candidate = false;
    options.min_region_faces = 2;

    const auto result = controller_.mergePlaneCandidates(candidates, options);
    const auto report = QString("%1%2\n输入候选数量：%3\n失败原因：%4\n消息：%5\n合并前 face：%6\n合并后 face：%7\nface reduction ratio：%8%\n合并前 edge：%9\n合并后 edge：%10\nedge reduction ratio：%11%\nmax deviation：%12\nmean deviation：%13\nrms deviation：%14\nBRepCheck：%15")
        .arg(title)
        .arg(result.success ? "完成" : "失败")
        .arg(candidates.size())
        .arg(regionMergeFailureText(result.failure_reason))
        .arg(QString::fromStdString(result.message))
        .arg(result.face_count_before)
        .arg(result.face_count_after)
        .arg(QString::number(result.face_reduction_ratio * 100.0, 'f', 2))
        .arg(result.edge_count_before)
        .arg(result.edge_count_after)
        .arg(QString::number(result.edge_reduction_ratio * 100.0, 'f', 2))
        .arg(QString::number(result.max_deviation, 'g', 6))
        .arg(QString::number(result.mean_deviation, 'g', 6))
        .arg(QString::number(result.rms_deviation, 'g', 6))
        .arg(result.brep_check_valid ? "通过" : "失败");

    inspectPanel_->showReport(report);
    if (!result.success) {
        logPanel_->appendWarning(QString("%1失败：原因 %2，%3")
            .arg(title)
            .arg(regionMergeFailureText(result.failure_reason))
            .arg(QString::fromStdString(result.message)));
        setStatus(QString("%1失败").arg(title));
        refreshUndoRedoActions();
        return;
    }

    refreshDocumentViews();
    logPanel_->appendInfo(QString("%1完成：输入候选 %2，face %3 -> %4，edge %5 -> %6")
        .arg(title)
        .arg(candidates.size())
        .arg(result.face_count_before)
        .arg(result.face_count_after)
        .arg(result.edge_count_before)
        .arg(result.edge_count_after));
    setStatus(QString("%1完成").arg(title));
    refreshUndoRedoActions();
}

void MainWindow::applyMerge() {
    if (!controller_.hasDocument()) {
        inspectPanel_->showReport("请先打开 STEP/STP 文件。");
        setStatus("未加载模型");
        return;
    }

    const auto params = parameterPanel_->parameters();
    const auto result = controller_.unifySameDomain(
        params.angular_threshold_degrees,
        params.min_edge_length,
        params.linear_tolerance,
        params.concat_bsplines);

    const auto& document = controller_.document();
    clearMergeCandidateState();
    hasFeatureEdgeResult_ = false;
    refreshModelTree();
    viewer_->displayDocument(document);
    syncLockedEdges();
    inspectPanel_->showReport(QString("同域合并完成\nconcat_bsplines：%1\n保护边数量：%2\n合并前 face：%3\n合并后 face：%4\nface reduction ratio：%5%\n合并前 edge：%6\n合并后 edge：%7\nedge reduction ratio：%8%\n合并前 solid：%9\n合并后 solid：%10")
        .arg(result.concat_bsplines ? "true" : "false")
        .arg(result.protected_edges)
        .arg(result.before.faces)
        .arg(result.after.faces)
        .arg(QString::number(result.face_reduction_ratio * 100.0, 'f', 2))
        .arg(result.before.edges)
        .arg(result.after.edges)
        .arg(QString::number(result.edge_reduction_ratio * 100.0, 'f', 2))
        .arg(result.before.solids)
        .arg(result.after.solids));
    logPanel_->appendInfo(QString("同域合并完成：concat_bsplines=%1，face %2 -> %3（%4%），edge %5 -> %6（%7%），保护边 %8")
        .arg(result.concat_bsplines ? "true" : "false")
        .arg(result.before.faces)
        .arg(result.after.faces)
        .arg(QString::number(result.face_reduction_ratio * 100.0, 'f', 2))
        .arg(result.before.edges)
        .arg(result.after.edges)
        .arg(QString::number(result.edge_reduction_ratio * 100.0, 'f', 2))
        .arg(result.protected_edges));
    setStatus("同域合并完成");
    refreshUndoRedoActions();
}

void MainWindow::validateShape() {
    if (!controller_.hasDocument()) {
        inspectPanel_->showValidation("当前没有已加载的模型。");
        return;
    }

    const auto report = controller_.validateShape();
    inspectPanel_->showValidation(QString("模型合法性检查完成\nOCCT BRepCheck：%1\n实体数：%2\n壳数：%3\n面数：%4\n边数：%5\n自由边数：%6\n多重边数：%7")
        .arg(report.brep_check_valid ? "通过" : "失败")
        .arg(report.stats.solids)
        .arg(report.stats.shells)
        .arg(report.stats.faces)
        .arg(report.stats.edges)
        .arg(report.free_edges)
        .arg(report.multiple_edges));
    bottomTabs_->setCurrentWidget(inspectPanel_->validationWidget());
    logPanel_->appendInfo(QString("合法性检查完成：free edge %1，multiple edge %2，BRepCheck %3")
        .arg(report.free_edges)
        .arg(report.multiple_edges)
        .arg(report.brep_check_valid ? "通过" : "失败"));
    setStatus("合法性检查完成");
    refreshUndoRedoActions();
}

void MainWindow::resetView() {
    viewer_->resetView();
    setStatus("视角已重置");
}

void MainWindow::undo() {
    const auto result = controller_.undo();
    if (!result.success()) {
        logPanel_->appendWarning(QString::fromStdString(result.message()));
        setStatus(QString::fromStdString(result.message()));
        refreshUndoRedoActions();
        return;
    }

    refreshDocumentViews();
    logPanel_->appendInfo("已撤销上一条编辑命令。");
    setStatus("已撤销");
    refreshUndoRedoActions();
}

void MainWindow::redo() {
    const auto result = controller_.redo();
    if (!result.success()) {
        logPanel_->appendWarning(QString::fromStdString(result.message()));
        setStatus(QString::fromStdString(result.message()));
        refreshUndoRedoActions();
        return;
    }

    refreshDocumentViews();
    logPanel_->appendInfo("已重做上一条编辑命令。");
    setStatus("已重做");
    refreshUndoRedoActions();
}

void MainWindow::refreshUndoRedoActions() {
    undoAction_->setEnabled(controller_.canUndo());
    redoAction_->setEnabled(controller_.canRedo());
}

void MainWindow::refreshDocumentViews() {
    if (!controller_.hasDocument()) {
        return;
    }

    const auto& document = controller_.document();
    clearMergeCandidateState();
    hasFeatureEdgeResult_ = false;
    refreshModelTree();
    viewer_->displayDocument(document);
    syncLockedEdges();
}

void MainWindow::syncLockedEdges() {
    if (!controller_.hasDocument()) {
        return;
    }

    refreshModelTree();
    viewer_->showLockedEdges(controller_.lockedEdges());
}

void MainWindow::refreshModelTree() {
    if (!controller_.hasDocument()) {
        return;
    }

    const auto counts = countCandidateStatuses(lastMergeCandidates_);
    modelTree_->showDocument(
        controller_.document(),
        static_cast<int>(controller_.lockedEdges().size()),
        static_cast<int>(lastMergeCandidates_.size()),
        visibleMergeCandidateCount_,
        counts.pending,
        counts.accepted,
        counts.rejected,
        counts.hidden,
        currentMergeCandidateId_,
        hasFeatureEdgeResult_ ? &controller_.featureEdges() : nullptr);
}

void MainWindow::clearMergeCandidateState() {
    lastMergeCandidates_.clear();
    visibleMergeCandidateIds_.clear();
    visibleMergeCandidateCount_ = 0;
    currentMergeCandidateId_ = -1;
    if (viewer_ != nullptr) {
        viewer_->clearMergeCandidates();
    }
}

void MainWindow::showFaceInspectReport(const FaceInspectInfo& info, bool hasCandidatePreview) {
    if (!info.valid) {
        inspectPanel_->showReport(QString("Face Inspect\nFace ID：%1\nSurface Type：Unknown\nCandidate State：InvalidFace")
            .arg(info.face_id));
        bottomTabs_->setCurrentWidget(inspectPanel_->reportWidget());
        return;
    }

    QString report = QString("Face Inspect\nFace ID：%1\nSurface Type：%2\nCandidate State：%3")
        .arg(info.face_id)
        .arg(QString::fromStdString(info.surface_type))
        .arg(faceInspectCandidateStateText(info.candidate_state));

    if (info.candidate_state == FaceInspectCandidateState::InVisibleCandidate ||
        info.candidate_state == FaceInspectCandidateState::InHiddenCandidate ||
        info.candidate_state == FaceInspectCandidateState::InCandidateButNotDisplayed) {
        report += QString("\nCandidate ID：%1\nCandidate Type：%2\nCandidate Status：%3\nRisk Level：%4\nFace Count：%5\nBoundary Edges：%6\nInternal Edges：%7\nMax Normal Angle：%8\nMax Distance：%9")
            .arg(info.candidate_id)
            .arg(candidateTypeText(info.candidate_type))
            .arg(candidateStatusText(info.candidate_status))
            .arg(riskLevelText(info.risk_level))
            .arg(info.candidate_face_count)
            .arg(info.candidate_boundary_edge_count)
            .arg(info.candidate_internal_edge_count)
            .arg(QString::number(info.max_normal_angle_deg, 'f', 3))
            .arg(QString::number(info.max_distance, 'g', 6));
    } else {
        report += QString("\nAdjacent Protected Edges：%1\nAdjacent Locked Edges：%2")
            .arg(info.adjacent_protected_edge_count)
            .arg(info.adjacent_locked_edge_count);
        if (!hasCandidatePreview) {
            report += "\nNote：当前尚未生成合并候选，请先点击“预览合并”。";
        } else {
            report += "\nNote：当前候选检测主要基于 PlaneLike。如果该区域是 Cylinder / Cone / Sphere / BSpline，可能需要 Stage 2.8 多类型候选识别。也可能因为 min_region_faces 不足或邻接 protected/locked edge 未形成候选。";
        }
    }

    if (info.candidate_state == FaceInspectCandidateState::InCandidateButNotDisplayed) {
        report += "\nNote：该 face 属于候选，但当前没有显示，可能被 Top N 显示过滤或当前筛选条件隐藏。";
    } else if (info.candidate_state == FaceInspectCandidateState::InHiddenCandidate) {
        report += "\nNote：该 face 属于 Hidden candidate，当前不显示该候选。";
    }

    inspectPanel_->showReport(report);
    bottomTabs_->setCurrentWidget(inspectPanel_->reportWidget());
}

MergeCandidate* MainWindow::currentMergeCandidate() {
    if (currentMergeCandidateId_ < 0) {
        return nullptr;
    }

    const auto it = std::find_if(lastMergeCandidates_.begin(), lastMergeCandidates_.end(), [this](const auto& candidate) {
        return candidate.candidate_id == currentMergeCandidateId_;
    });
    if (it == lastMergeCandidates_.end()) {
        return nullptr;
    }
    return &(*it);
}

bool MainWindow::setCurrentMergeCandidateStatus(MergeCandidateStatus status) {
    auto* candidate = currentMergeCandidate();
    if (candidate == nullptr) {
        logPanel_->appendWarning("请先选择一个候选区域。");
        setStatus("未选择候选区域");
        return false;
    }

    candidate->status = status;
    if (status == MergeCandidateStatus::Accepted || status == MergeCandidateStatus::Pending) {
        viewer_->showMergeCandidateById(std::vector<MergeCandidate>{*candidate}, candidate->candidate_id);
        visibleMergeCandidateCount_ = 1;
        visibleMergeCandidateIds_.clear();
        visibleMergeCandidateIds_.insert(candidate->candidate_id);
        if (hasFeatureEdgeResult_) {
            viewer_->showFeatureEdges(controller_.featureEdges());
        }
    } else {
        visibleMergeCandidateIds_.clear();
    }
    refreshModelTree();
    return true;
}

void MainWindow::showFilteredMergeCandidates(MergeCandidateStatus status) {
    if (lastMergeCandidates_.empty()) {
        logPanel_->appendWarning("当前没有候选区域，请先点击“预览合并”。");
        setStatus("没有候选区域");
        return;
    }

    std::vector<MergeCandidate> filteredCandidates;
    for (const auto& candidate : lastMergeCandidates_) {
        if (candidate.status == status) {
            filteredCandidates.push_back(candidate);
        }
    }

    viewer_->showMergeCandidates(filteredCandidates, 10, true);
    if (hasFeatureEdgeResult_) {
        viewer_->showFeatureEdges(controller_.featureEdges());
    }
    visibleMergeCandidateCount_ = static_cast<int>(filteredCandidates.size());
    visibleMergeCandidateIds_.clear();
    for (const auto& candidate : filteredCandidates) {
        addVisibleCandidateId(visibleMergeCandidateIds_, candidate);
    }
    refreshModelTree();
    showCandidateStatusReport(QString("已显示 %1 候选区域").arg(candidateStatusText(status)));
    setStatus(QString("已显示 %1 候选区域").arg(candidateStatusText(status)));
}

void MainWindow::showCandidateStatusReport(const QString& title) {
    const auto counts = countCandidateStatuses(lastMergeCandidates_);
    QString currentStatus = "无";
    if (const auto* candidate = currentMergeCandidate()) {
        currentStatus = candidateStatusText(candidate->status);
    }

    inspectPanel_->showReport(QString("%1\n当前候选 ID：%2\n当前候选状态：%3\nPending：%4\nAccepted：%5\nRejected：%6\nHidden：%7\n当前显示候选数量：%8")
        .arg(title)
        .arg(currentMergeCandidateId_ >= 0 ? QString::number(currentMergeCandidateId_) : "无")
        .arg(currentStatus)
        .arg(counts.pending)
        .arg(counts.accepted)
        .arg(counts.rejected)
        .arg(counts.hidden)
        .arg(visibleMergeCandidateCount_));
    logPanel_->appendInfo(QString("%1：Pending %2，Accepted %3，Rejected %4，Hidden %5，当前显示 %6")
        .arg(title)
        .arg(counts.pending)
        .arg(counts.accepted)
        .arg(counts.rejected)
        .arg(counts.hidden)
        .arg(visibleMergeCandidateCount_));
}

void MainWindow::lockSelectedEdges(const std::vector<EdgeId>& edgeIds) {
    if (edgeIds.empty()) {
        setStatus("未选择边");
        return;
    }

    const auto result = controller_.lockEdges(edgeIds);
    if (!result.success()) {
        logPanel_->appendError(QString::fromStdString(result.message()));
        setStatus(QString::fromStdString(result.message()));
        refreshUndoRedoActions();
        return;
    }

    logPanel_->appendInfo(QString("已锁定 %1 条边。").arg(edgeIds.size()));
    setStatus(QString("已锁定 %1 条边").arg(edgeIds.size()));
    syncLockedEdges();
    refreshUndoRedoActions();
}

void MainWindow::unlockSelectedEdges(const std::vector<EdgeId>& edgeIds) {
    if (edgeIds.empty()) {
        setStatus("未选择边");
        return;
    }

    const auto result = controller_.unlockEdges(edgeIds);
    if (!result.success()) {
        logPanel_->appendError(QString::fromStdString(result.message()));
        setStatus(QString::fromStdString(result.message()));
        refreshUndoRedoActions();
        return;
    }

    logPanel_->appendInfo(QString("已解锁 %1 条边。").arg(edgeIds.size()));
    setStatus(QString("已解锁 %1 条边").arg(edgeIds.size()));
    syncLockedEdges();
    refreshUndoRedoActions();
}

void MainWindow::setStatus(const QString& message) {
    statusBar()->showMessage(message);
}

}
