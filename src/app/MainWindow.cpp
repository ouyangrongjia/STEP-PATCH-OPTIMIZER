#include "app/MainWindow.h"

#include "gui/InspectPanel.h"
#include "gui/LogPanel.h"
#include "gui/ModelTreePanel.h"
#include "gui/OccViewWidget.h"
#include "gui/ParameterPanel.h"

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
    showAllMergeCandidatesAction_ = new QAction("显示全部候选", this);
    highlightMergeCandidateByIdAction_ = new QAction("按 ID 高亮候选", this);
    clearMergeCandidatesAction_ = new QAction("清除候选高亮", this);
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
    lastMergeCandidates_.clear();
    visibleMergeCandidateCount_ = 0;
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
    lastMergeCandidates_ = result.candidates;
    hasFeatureEdgeResult_ = true;

    int maxFaceCount = 0;
    int totalCandidateFaces = 0;
    for (const auto& candidate : result.candidates) {
        maxFaceCount = std::max(maxFaceCount, candidate.face_count);
        totalCandidateFaces += candidate.face_count;
    }

    QString report = QString("合并候选区域预览完成\n候选区域数量：%1\n保护边数量：%2\n访问 face 数量：%3\n拒绝区域数量：%4\n最大候选区域 face 数：%5\n总候选 face 数：%6\n预览前 face/edge：%7/%8\n预览后 face/edge：%9/%10")
        .arg(result.candidates.size())
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
        report += QString("\n\n候选 %1\n类型：%2\nface 数：%3\n总面积：%4\n最大法向夹角：%5 度\n最大距离：%6\n边界边数：%7\n内部边数：%8\n风险：%9")
            .arg(candidate.candidate_id)
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
        report += "\n\n没有可预览候选区域。";
    } else {
        viewer_->showMergeCandidates(result.candidates, 10, false);
        visibleMergeCandidateCount_ = static_cast<int>(std::min<std::size_t>(10, result.candidates.size()));
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

    viewer_->showMergeCandidates(lastMergeCandidates_, 10, true);
    if (hasFeatureEdgeResult_) {
        viewer_->showFeatureEdges(controller_.featureEdges());
    }
    visibleMergeCandidateCount_ = static_cast<int>(lastMergeCandidates_.size());
    refreshModelTree();
    logPanel_->appendInfo(QString("已显示全部 %1 个候选区域。").arg(lastMergeCandidates_.size()));
    setStatus("已显示全部候选区域");
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

    if (!viewer_->showMergeCandidateById(lastMergeCandidates_, candidateId)) {
        visibleMergeCandidateCount_ = 0;
        refreshModelTree();
        logPanel_->appendWarning(QString("未找到候选 ID：%1").arg(candidateId));
        setStatus("未找到候选区域");
        return;
    }

    if (hasFeatureEdgeResult_) {
        viewer_->showFeatureEdges(controller_.featureEdges());
    }
    visibleMergeCandidateCount_ = 1;
    refreshModelTree();
    logPanel_->appendInfo(QString("已高亮候选 ID：%1").arg(candidateId));
    setStatus(QString("已高亮候选 %1").arg(candidateId));
}

void MainWindow::clearMergeCandidatePreview() {
    viewer_->clearMergeCandidates();
    visibleMergeCandidateCount_ = 0;
    refreshModelTree();
    logPanel_->appendInfo("已清除候选区域高亮。");
    setStatus("候选高亮已清除");
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
    lastMergeCandidates_.clear();
    visibleMergeCandidateCount_ = 0;
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
    lastMergeCandidates_.clear();
    visibleMergeCandidateCount_ = 0;
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

    modelTree_->showDocument(
        controller_.document(),
        static_cast<int>(controller_.lockedEdges().size()),
        static_cast<int>(lastMergeCandidates_.size()),
        visibleMergeCandidateCount_,
        hasFeatureEdgeResult_ ? &controller_.featureEdges() : nullptr);
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
