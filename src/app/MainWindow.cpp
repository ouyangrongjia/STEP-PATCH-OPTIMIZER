#include "app/MainWindow.h"

#include "gui/InspectPanel.h"
#include "gui/LogPanel.h"
#include "gui/ModelTreePanel.h"
#include "gui/OccViewWidget.h"
#include "gui/ParameterPanel.h"
#include "gui/ProcessStatusPanel.h"
#include "io/StlReader.h"
#include "merge/CandidateFilters.h"
#include "merge/FaceInspector.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QDockWidget>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QStatusBar>
#include <QStringList>
#include <QTabWidget>
#include <QTimer>
#include <QList>
#include <QSize>
#include <QToolBar>
#include <QToolButton>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <utility>

namespace spo {

namespace {

std::filesystem::path pathFromQString(const QString& path) {
    return std::filesystem::path(path.toStdWString());
}

QString pathToQString(const std::filesystem::path& path) {
    return QString::fromStdWString(path.wstring());
}

QString elapsedText(qint64 milliseconds) {
    const auto seconds = milliseconds / 1000;
    return QString("%1:%2")
        .arg(seconds / 60)
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QString candidateTypeText(MergeCandidateType type) {
    switch (type) {
    case MergeCandidateType::SameDomain:
        return "SameDomain";
    case MergeCandidateType::FeatureBoundedRefit:
        return "FeatureBoundedRefit";
    case MergeCandidateType::Unknown:
        return "Unknown";
    }
    return "Unknown";
}

std::vector<MergeCandidateType> displayCandidateTypes() {
    return {
        MergeCandidateType::FeatureBoundedRefit,
        MergeCandidateType::Unknown
    };
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

QString stlBoundingBoxText(const StlBoundingBox& bbox) {
    if (!bbox.valid) {
        return "invalid";
    }
    return QString("min=(%1, %2, %3), max=(%4, %5, %6)")
        .arg(QString::number(bbox.min.x, 'g', 8))
        .arg(QString::number(bbox.min.y, 'g', 8))
        .arg(QString::number(bbox.min.z, 'g', 8))
        .arg(QString::number(bbox.max.x, 'g', 8))
        .arg(QString::number(bbox.max.y, 'g', 8))
        .arg(QString::number(bbox.max.z, 'g', 8));
}

QString bboxText(
    bool valid,
    double minX,
    double minY,
    double minZ,
    double maxX,
    double maxY,
    double maxZ) {
    if (!valid) {
        return "invalid";
    }
    return QString("min=(%1, %2, %3), max=(%4, %5, %6)")
        .arg(QString::number(minX, 'g', 8))
        .arg(QString::number(minY, 'g', 8))
        .arg(QString::number(minZ, 'g', 8))
        .arg(QString::number(maxX, 'g', 8))
        .arg(QString::number(maxY, 'g', 8))
        .arg(QString::number(maxZ, 'g', 8));
}

QString boolText(bool value) {
    return value ? "true" : "false";
}

struct PatchApplyUiResult {
    bool success = false;
    std::string resultMessage;
    std::string statusMessage;
    PatchReplacementReport report;
};

QString patchApplyReportText(const PatchReplacementReport& report, const QString& statusMessage) {
    QStringList reportLines;
    reportLines << "Patch Apply";
    if (report.success) {
        reportLines << QString("✅ 成功  |  候选 %1  |  替换 %2 面 → %3 面  |  BRepCheck 通过  |  STEP roundtrip 通过")
            .arg(report.candidateId)
            .arg(report.sourceFaceCount)
            .arg(report.replacementFaceCount);
        if (report.usedMultiSurfaceBoundaryShell) {
            reportLines << "策略：multi-surface boundary shell";
        } else if (report.usedOriginalBoundarySurfaceRetrim) {
            reportLines << "策略：original boundary surface retrim";
        }
    } else {
        reportLines << QString("❌ 失败  |  候选 %1  |  原因：%2")
            .arg(report.candidateId)
            .arg(QString::fromStdString(report.gateFailureReason.empty()
                ? toString(report.failureReason) : report.gateFailureReason));
    }
    reportLines << QString("缝合容差 %1（%2 次尝试）| 最佳缝合 free edge %3  |  multiple edge %4  |  BRepCheck %5")
        .arg(QString::number(report.selectedSewingTolerance, 'g', 4))
        .arg(report.sewingAttemptCount)
        .arg(report.bestSewingFreeEdges)
        .arg(report.bestSewingMultipleEdges)
        .arg(boolText(report.bestSewingBRepCheckValid));
    reportLines << QString("最佳缝合退化 free edge %1  |  repair 退化 free edge %2 → %3")
        .arg(report.bestSewingDegeneratedFreeEdgeCount)
        .arg(report.degeneratedFreeEdgesBeforeRepair)
        .arg(report.degeneratedFreeEdgesAfterRepair);
    reportLines << QString("修复拓扑：free edge 修复前 %1 → 修复后 %2  |  multiple edge 修复前 %3 → 修复后 %4")
        .arg(report.freeEdgesBeforeRepair)
        .arg(report.freeEdgesAfterRepair)
        .arg(report.multipleEdgesBeforeRepair)
        .arg(report.multipleEdgesAfterRepair);
    reportLines << QString("Pre-repair closure：BRepCheck %1  |  face/edge/shell/solid %2/%3/%4/%5  |  free %6  |  multiple %7")
        .arg(boolText(report.preRepairBRepCheckValid))
        .arg(report.preRepairFaceCount)
        .arg(report.preRepairEdgeCount)
        .arg(report.preRepairShellCount)
        .arg(report.preRepairSolidCount)
        .arg(report.preRepairFreeEdgeCount)
        .arg(report.preRepairMultipleEdgeCount);
    reportLines << QString("Pre/Post 退化 free edge：pre %1  |  post %2  |  appeared-after-repair %3")
        .arg(report.preRepairDegeneratedFreeEdgeCount)
        .arg(report.postRepairDegeneratedFreeEdgeCount)
        .arg(report.appearedAfterRepairDegeneratedFreeEdgeCount);
    reportLines << QString("Post-repair closure：BRepCheck %1  |  face/edge/shell/solid %2/%3/%4/%5  |  free %6  |  multiple %7")
        .arg(boolText(report.postRepairBRepCheckValid))
        .arg(report.postRepairFaceCount)
        .arg(report.postRepairEdgeCount)
        .arg(report.postRepairShellCount)
        .arg(report.postRepairSolidCount)
        .arg(report.postRepairFreeEdgeCount)
        .arg(report.postRepairMultipleEdgeCount);
    if (!report.freeEdgeDiagnostics.empty()) {
        const auto& diagnostic = report.freeEdgeDiagnostics.front();
        reportLines << QString("Free edge 定位：数量 %1  |  first edge %2  |  appeared_after_repair %3  |  adjacent faces %4  |  original boundary edge %5  |  patch face owner %6")
            .arg(static_cast<int>(report.freeEdgeDiagnostics.size()))
            .arg(diagnostic.edgeIndex)
            .arg(boolText(diagnostic.appearedAfterRepair))
            .arg(diagnostic.adjacentFaceCount)
            .arg(diagnostic.nearestOriginalBoundaryEdgeId)
            .arg(diagnostic.patchFaceOwner);
        reportLines << QString("Free edge 局部：length %1  |  tolerance %2  |  degenerated %3  |  nearest split edge %4  |  split distance %5  |  projection max %6")
            .arg(QString::number(diagnostic.edgeLength, 'g', 8))
            .arg(QString::number(diagnostic.edgeTolerance, 'g', 8))
            .arg(boolText(diagnostic.degenerated))
            .arg(diagnostic.nearestSplitBoundaryOriginalEdgeId)
            .arg(QString::number(diagnostic.nearestSplitBoundarySegmentDistance, 'g', 8))
            .arg(QString::number(diagnostic.fittedPatchProjectionMaxDistance, 'g', 8));
        if (diagnostic.midpointValid) {
            reportLines << QString("Free edge midpoint：(%1, %2, %3)")
                .arg(QString::number(diagnostic.midpointX, 'g', 8))
                .arg(QString::number(diagnostic.midpointY, 'g', 8))
                .arg(QString::number(diagnostic.midpointZ, 'g', 8));
        }
    }
    if (report.trimDiagnostics.captured) {
        reportLines << QString("B2.7 裁剪诊断：faces %1  |  invalid wires %2  |  over-cover %3/%4 max %5  |  under-cover %6/%7 max %8")
            .arg(report.trimDiagnostics.replacementFaceCount)
            .arg(report.trimDiagnostics.trimWireInvalidCount)
            .arg(report.trimDiagnostics.overCoverSampleCount)
            .arg(report.trimDiagnostics.overCoverTotalSampleCount)
            .arg(QString::number(report.trimDiagnostics.overCoverMaxDistance, 'g', 8))
            .arg(report.trimDiagnostics.underCoverSampleCount)
            .arg(report.trimDiagnostics.underCoverTotalSampleCount)
            .arg(QString::number(report.trimDiagnostics.underCoverMaxDistance, 'g', 8));
        reportLines << QString("B2.7 缝隙诊断：boundary gap max/p95/rms %1/%2/%3 edge %4  |  internal seam max/p95/rms %5/%6/%7 edge %8  |  roundtrip changed %9")
            .arg(QString::number(report.trimDiagnostics.boundaryGapMax, 'g', 8))
            .arg(QString::number(report.trimDiagnostics.boundaryGapP95, 'g', 8))
            .arg(QString::number(report.trimDiagnostics.boundaryGapRms, 'g', 8))
            .arg(report.trimDiagnostics.worstBoundaryEdgeId)
            .arg(QString::number(report.trimDiagnostics.internalSeamGapMax, 'g', 8))
            .arg(QString::number(report.trimDiagnostics.internalSeamGapP95, 'g', 8))
            .arg(QString::number(report.trimDiagnostics.internalSeamGapRms, 'g', 8))
            .arg(report.trimDiagnostics.worstInternalEdgeId)
            .arg(boolText(report.trimDiagnostics.roundtripChanged));
    }
    if (report.externalCadDiagnostics.captured) {
        reportLines << QString("B2.8 外部 CAD 诊断路由：原始补片预诊断 %1/%2  |  最终诊断 %3/%4  |  阶段 %5")
            .arg(boolText(report.externalCadDiagnostics.rawPatchPreflightAvailable))
            .arg(QString::fromStdString(report.externalCadDiagnostics.rawPatchPreflightStatus))
            .arg(boolText(report.externalCadDiagnostics.finalAppliedStepDiagnosticEligible))
            .arg(QString::fromStdString(report.externalCadDiagnostics.finalAppliedStepDiagnosticStatus))
            .arg(QString::fromStdString(report.externalCadDiagnostics.finalAppliedStepDiagnosticStage));
        if (!report.externalCadDiagnostics.finalAppliedStepDiagnosticSkippedReason.empty()) {
            reportLines << QString("B2.8 跳过原因：%1")
                .arg(QString::fromStdString(report.externalCadDiagnostics.finalAppliedStepDiagnosticSkippedReason));
        }
    }
    reportLines << QString("Gate free edge：before %1  |  after %2  |  STEP roundtrip %3")
        .arg(report.gateBeforeFreeEdges)
        .arg(report.gateAfterFreeEdges)
        .arg(report.gateRoundtripFreeEdges);
    reportLines << QString("Gate multiple edge：before %1  |  after %2  |  STEP roundtrip %3")
        .arg(report.gateBeforeMultipleEdges)
        .arg(report.gateAfterMultipleEdges)
        .arg(report.gateRoundtripMultipleEdges);
    reportLines << QString("Gate BRepCheck：before %1  |  after %2  |  STEP roundtrip %3")
        .arg(boolText(report.gateBeforeBRepCheckValid))
        .arg(boolText(report.gateAfterBRepCheckValid))
        .arg(boolText(report.gateRoundtripBRepCheckValid));
    reportLines << QString("Gate faces/edges/shells/solids：before %1/%2/%3/%4  |  after %5/%6/%7/%8  |  STEP roundtrip %9/%10/%11/%12")
        .arg(report.gateBeforeFaceCount)
        .arg(report.gateBeforeEdgeCount)
        .arg(report.gateBeforeShellCount)
        .arg(report.gateBeforeSolidCount)
        .arg(report.gateAfterFaceCount)
        .arg(report.gateAfterEdgeCount)
        .arg(report.gateAfterShellCount)
        .arg(report.gateAfterSolidCount)
        .arg(report.gateRoundtripFaceCount)
        .arg(report.gateRoundtripEdgeCount)
        .arg(report.gateRoundtripShellCount)
        .arg(report.gateRoundtripSolidCount);
    reportLines << QString("Gate %1")
        .arg(report.gatePassed ? "通过" : QString("不通过：%1").arg(QString::fromStdString(report.gateFailureReason)));
    const auto warn = QString::fromStdString(report.warningMessage);
    if (!warn.isEmpty()) {
        reportLines << QString("警告：%1").arg(warn);
    }
    reportLines << statusMessage;
    return reportLines.join('\n');
}

struct OpenStepUiResult {
    bool success = false;
    std::string message;
    std::filesystem::path path;
};

struct ExportStepUiResult {
    bool exportSuccess = false;
    std::string exportMessage;
    bool verifySuccess = false;
    std::string verifyMessage;
    std::filesystem::path path;
};

struct MergePreviewUiResult {
    MergePlannerResult result;
    ShapeStats beforeStats;
    ShapeStats afterStats;
};

QString gapEdgeIdsText(const std::vector<EdgeId>& edgeIds) {
    if (edgeIds.empty()) {
        return "none";
    }

    QStringList ids;
    for (const auto edgeId : edgeIds) {
        ids << QString::number(edgeId);
    }
    return ids.join(", ");
}

QString cropBoundaryDiagnosticsText(const CropBoundaryDiagnosticsReport& report) {
    QStringList lines;
    lines << "边界诊断";
    if (report.suspectedGapCount > 0) {
        lines << QString("疑似缺口：%1 段，边 %2")
            .arg(report.suspectedGapCount)
            .arg(gapEdgeIdsText(report.suspectedGapEdgeIds));
    }
    if (report.stlCoverageMissingPointCount > 0) {
        lines << QString("STL 覆盖不足：%1 点缺失，最大偏差 %2")
            .arg(report.stlCoverageMissingPointCount)
            .arg(QString::number(report.stlCoverageMaxDistance, 'g', 4));
    }
    const auto msg = QString::fromStdString(report.message);
    const auto warn = QString::fromStdString(report.warningMessage);
    if (!msg.isEmpty()) lines << QString("信息：%1").arg(msg);
    if (!warn.isEmpty()) lines << QString("警告：%1").arg(warn);
    return lines.join('\n');
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
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks);
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);

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
    if (event->type() == QEvent::Close && (patchApplyInProgress_ || stlCropInProgress_)) {
        event->ignore();
        QMessageBox::information(this, "后台任务", "当前后台任务正在运行，请等待任务结束。");
        return true;
    }
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

    openSourceStlAction_ = new QAction("打开原始 STL", this);
    cropCurrentCandidateStlAction_ = new QAction("裁剪当前候选 STL", this);
    useConservativeStlCropAction_ = new QAction("启用保守边界裁剪", this);
    useConservativeStlCropAction_->setCheckable(true);
    useConservativeStlCropAction_->setChecked(false);

    useGlobalCutChainCropAction_ = new QAction("启用全局切链裁剪 (Global Cut Chain)", this);
    useGlobalCutChainCropAction_->setCheckable(true);
    useGlobalCutChainCropAction_->setChecked(false);

    auto* stlCropModeGroup = new QActionGroup(this);
    stlCropModeGroup->setExclusive(true);
    stlCropModeGroup->addAction(useConservativeStlCropAction_);
    stlCropModeGroup->addAction(useGlobalCutChainCropAction_);

    fittingModeLegacyStlCropAction_ = new QAction("Legacy STL Crop", this);
    fittingModeLegacyStlCropAction_->setCheckable(true);
    fittingModeLegacyStlCropAction_->setChecked(false);
    fittingModeConservativeBandAction_ = new QAction("Conservative Boundary Band STL Crop", this);
    fittingModeConservativeBandAction_->setCheckable(true);
    fittingModeConservativeBandAction_->setChecked(false);
    fittingModeStpSampledAction_ = new QAction("STP Sampled Candidate Surface", this);
    fittingModeStpSampledAction_->setCheckable(true);
    fittingModeStpSampledAction_->setChecked(true);

    auto* fittingInputModeGroup = new QActionGroup(this);
    fittingInputModeGroup->setExclusive(true);
    fittingInputModeGroup->addAction(fittingModeLegacyStlCropAction_);
    fittingInputModeGroup->addAction(fittingModeConservativeBandAction_);
    fittingInputModeGroup->addAction(fittingModeStpSampledAction_);

    showSourceStlAction_ = new QAction("显示源 STL", this);
    showSourceStlAction_->setCheckable(true);
    showSourceStlAction_->setChecked(true);
    showSourceStlAction_->setEnabled(false);
    showCroppedStlAction_ = new QAction("显示裁剪 STL", this);
    showCroppedStlAction_->setCheckable(true);
    showCroppedStlAction_->setChecked(true);
    showCroppedStlAction_->setEnabled(false);
    showStlCropBoxAction_ = new QAction("显示裁剪 bbox", this);
    showStlCropBoxAction_->setCheckable(true);
    showStlCropBoxAction_->setChecked(false);
    showStlCropBoxAction_->setEnabled(false);
    generateAndPreviewCurrentPatchAction_ = new QAction("生成并预览当前 Patch", this);
    useGeomagicRemeshAction_ = new QAction("启用 Geomagic Remesh", this);
    useGeomagicRemeshAction_->setCheckable(true);
    useGeomagicRemeshAction_->setChecked(false);
    importPatchForCurrentCandidateAction_ = new QAction("从 local STL 定位并导入 Patch", this);
    importPatchFromFileAction_ = new QAction("从文件导入 Patch", this);
    applyCurrentPatchAction_ = new QAction("应用当前 Patch 到候选区域", this);
    applyCurrentPatchAction_->setEnabled(false);
    clearPatchOverlayAction_ = new QAction("清除 Patch Overlay", this);

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
    showCandidatesByTypeAction_ = new QAction("按类型显示候选", this);
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
    viewMenu_->addSeparator();
    viewMenu_->addAction(toggleFeaturesAction_);
    viewMenu_->addAction(resetViewAction_);
    viewMenu_->addAction(fitAllAction_);

    auto* detectMenu = menuBar()->addMenu("检测");
    detectMenu->addAction(detectAction_);

    auto* validateMenu = menuBar()->addMenu("验证");
    validateMenu->addAction(validateAction_);

    auto* exportMenu = menuBar()->addMenu("导出");
    exportMenu->addAction(exportStepAction_);

    auto* advancedMenu = menuBar()->addMenu("高级");

    auto* candidateMenu = advancedMenu->addMenu("候选区域");
    candidateMenu->addAction(selectCandidateAction_);
    candidateMenu->addSeparator();
    candidateMenu->addAction(previewMergeAction_);
    candidateMenu->addAction(showAllMergeCandidatesAction_);
    candidateMenu->addAction(highlightMergeCandidateByIdAction_);
    candidateMenu->addAction(clearMergeCandidatesAction_);
    candidateMenu->addSeparator();
    candidateMenu->addAction(acceptMergeCandidateAction_);
    candidateMenu->addAction(rejectMergeCandidateAction_);
    candidateMenu->addAction(hideMergeCandidateAction_);
    candidateMenu->addAction(restoreMergeCandidateAction_);
    candidateMenu->addSeparator();
    candidateMenu->addAction(showAcceptedMergeCandidatesAction_);
    candidateMenu->addAction(showPendingMergeCandidatesAction_);
    candidateMenu->addAction(showCandidatesByTypeAction_);

    stlMenu_ = advancedMenu->addMenu("STL 裁剪");
    stlMenu_->addAction(openSourceStlAction_);
    stlMenu_->addAction(cropCurrentCandidateStlAction_);
    stlMenu_->addAction(useConservativeStlCropAction_);
    stlMenu_->addAction(useGlobalCutChainCropAction_);
    stlMenu_->addSeparator();
    stlMenu_->addAction(showSourceStlAction_);
    stlMenu_->addAction(showCroppedStlAction_);
    stlMenu_->addAction(showStlCropBoxAction_);

    patchMenu_ = advancedMenu->addMenu("Patch / Geomagic");
    auto* fittingInputModeMenu = patchMenu_->addMenu("Geomagic Fitting Input Mode");
    fittingInputModeMenu->addAction(fittingModeLegacyStlCropAction_);
    fittingInputModeMenu->addAction(fittingModeConservativeBandAction_);
    fittingInputModeMenu->addAction(fittingModeStpSampledAction_);
    patchMenu_->addSeparator();
    patchMenu_->addAction(generateAndPreviewCurrentPatchAction_);
    patchMenu_->addAction(useGeomagicRemeshAction_);
    patchMenu_->addSeparator();
    patchMenu_->addAction(importPatchForCurrentCandidateAction_);
    patchMenu_->addAction(importPatchFromFileAction_);
    patchMenu_->addSeparator();
    patchMenu_->addAction(applyCurrentPatchAction_);
    patchMenu_->addSeparator();
    patchMenu_->addAction(clearPatchOverlayAction_);

    auto* mergeMenu = advancedMenu->addMenu("合并实验");
    mergeMenu->addAction(applyMergeAction_);
    mergeMenu->addSeparator();
    mergeMenu->addAction(undoAction_);
    mergeMenu->addAction(redoAction_);

    auto* helpMenu = menuBar()->addMenu("帮助");
    helpMenu->addAction("关于", this, [this]() {
        QMessageBox::about(this, "关于", "STEP 曲面片优化器\n特征感知的 STEP 曲面片合并与边界优化系统");
    });
}

void MainWindow::createToolBars() {
    auto* toolBar = addToolBar("主工具栏");
    toolBar->setMovable(false);
    toolBar->setObjectName("primaryToolBar");
    toolBar->setIconSize(QSize(18, 18));
    toolBar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    toolBar->setAllowedAreas(Qt::TopToolBarArea);

    auto* selectionMenu = new QMenu(this);
    selectionMenu->addAction(selectFaceAction_);
    selectionMenu->addAction(selectEdgeAction_);
    selectionMenu->addAction(selectCandidateAction_);

    auto* candidateToolMenu = new QMenu(this);
    candidateToolMenu->addAction(selectCandidateAction_);
    candidateToolMenu->addSeparator();
    candidateToolMenu->addAction(previewMergeAction_);
    candidateToolMenu->addAction(showAllMergeCandidatesAction_);
    candidateToolMenu->addAction(showAcceptedMergeCandidatesAction_);
    candidateToolMenu->addAction(showPendingMergeCandidatesAction_);
    candidateToolMenu->addAction(highlightMergeCandidateByIdAction_);
    candidateToolMenu->addSeparator();
    candidateToolMenu->addAction(acceptMergeCandidateAction_);
    candidateToolMenu->addAction(rejectMergeCandidateAction_);
    candidateToolMenu->addAction(hideMergeCandidateAction_);
    candidateToolMenu->addAction(restoreMergeCandidateAction_);

    auto* patchToolMenu = new QMenu(this);
    patchToolMenu->addAction(generateAndPreviewCurrentPatchAction_);
    patchToolMenu->addAction(applyCurrentPatchAction_);
    patchToolMenu->addSeparator();
    patchToolMenu->addAction(importPatchForCurrentCandidateAction_);
    patchToolMenu->addAction(importPatchFromFileAction_);
    patchToolMenu->addSeparator();
    auto* fittingInputModeToolMenu = patchToolMenu->addMenu("Geomagic Fitting Input Mode");
    fittingInputModeToolMenu->addAction(fittingModeLegacyStlCropAction_);
    fittingInputModeToolMenu->addAction(fittingModeConservativeBandAction_);
    fittingInputModeToolMenu->addAction(fittingModeStpSampledAction_);
    patchToolMenu->addAction(useGeomagicRemeshAction_);
    patchToolMenu->addSeparator();
    patchToolMenu->addAction(clearPatchOverlayAction_);

    auto* validateExportMenu = new QMenu(this);
    validateExportMenu->addAction(validateAction_);
    validateExportMenu->addAction(exportStepAction_);

    auto addMenuButton = [this, toolBar](const QString& text, QMenu* menu) {
        auto* button = new QToolButton(this);
        button->setText(text);
        button->setPopupMode(QToolButton::InstantPopup);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setMinimumHeight(30);
        button->setMenu(menu);
        toolBar->addWidget(button);
    };

    toolBar->addAction(openStepAction_);
    toolBar->addAction(saveProjectAction_);
    toolBar->addSeparator();
    addMenuButton("选择", selectionMenu);
    toolBar->addAction(detectAction_);
    addMenuButton("候选区域", candidateToolMenu);
    addMenuButton("Patch", patchToolMenu);
    toolBar->addAction(toggleFeaturesAction_);
    addMenuButton("检查/导出", validateExportMenu);
    toolBar->addSeparator();
    toolBar->addAction(undoAction_);
    toolBar->addAction(redoAction_);
}

void MainWindow::createDocks() {
    viewer_ = new OccViewWidget(this);
    viewer_->setObjectName("occViewport");
    setCentralWidget(viewer_);

    modelTree_ = new ModelTreePanel(this);
    modelDock_ = new QDockWidget("模型结构", this);
    modelDock_->setObjectName("modelTreeDock");
    modelDock_->setWidget(modelTree_);
    modelDock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    modelDock_->setMinimumWidth(220);
    addDockWidget(Qt::LeftDockWidgetArea, modelDock_);

    parameterPanel_ = new ParameterPanel(this);
    parameterDock_ = new QDockWidget("检测参数", this);
    parameterDock_->setObjectName("parameterDock");
    parameterDock_->setWidget(parameterPanel_);
    parameterDock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    parameterDock_->setMinimumWidth(240);
    parameterDock_->setMaximumWidth(360);
    addDockWidget(Qt::RightDockWidgetArea, parameterDock_);

    inspectPanel_ = new InspectPanel(this);
    logPanel_ = new LogPanel(this);
    processStatusPanel_ = new ProcessStatusPanel(this);

    bottomTabs_ = new QTabWidget(this);
    bottomTabs_->setObjectName("outputTabs");
    bottomTabs_->addTab(logPanel_, "日志");
    bottomTabs_->addTab(processStatusPanel_, "进程");
    bottomTabs_->addTab(inspectPanel_, "检查");
    bottomTabs_->addTab(inspectPanel_->validationWidget(), "验证");
    bottomTabs_->addTab(inspectPanel_->reportWidget(), "报告");

    bottomDock_ = new QDockWidget("输出与检查", this);
    bottomDock_->setObjectName("outputDock");
    bottomDock_->setWidget(bottomTabs_);
    bottomDock_->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    bottomDock_->setMinimumHeight(150);
    addDockWidget(Qt::BottomDockWidgetArea, bottomDock_);

    const QList<QDockWidget*> horizontalDocks{modelDock_, parameterDock_};
    const QList<int> horizontalSizes{260, 280};
    resizeDocks(horizontalDocks, horizontalSizes, Qt::Horizontal);

    const QList<QDockWidget*> verticalDocks{bottomDock_};
    const QList<int> verticalSizes{210};
    resizeDocks(verticalDocks, verticalSizes, Qt::Vertical);

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
    connect(openSourceStlAction_, &QAction::triggered, this, [this]() { openSourceStlFile(); });
    connect(cropCurrentCandidateStlAction_, &QAction::triggered, this, [this]() { cropCurrentCandidateStl(); });
    connect(generateAndPreviewCurrentPatchAction_, &QAction::triggered, this, [this]() { generateAndPreviewCurrentPatch(); });
    connect(importPatchForCurrentCandidateAction_, &QAction::triggered, this, [this]() { importPatchForCurrentCandidate(); });
    connect(importPatchFromFileAction_, &QAction::triggered, this, [this]() { importPatchFromFile(); });
    connect(applyCurrentPatchAction_, &QAction::triggered, this, [this]() { applyCurrentPatchPreview(); });
    connect(clearPatchOverlayAction_, &QAction::triggered, this, [this]() { clearPatchOverlay(); });
    connect(showSourceStlAction_, &QAction::toggled, viewer_, &OccViewWidget::setSourceStlVisible);
    connect(showCroppedStlAction_, &QAction::toggled, viewer_, &OccViewWidget::setCroppedStlVisible);
    connect(showStlCropBoxAction_, &QAction::toggled, viewer_, &OccViewWidget::setStlCropBoxVisible);
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
    connect(showCandidatesByTypeAction_, &QAction::triggered, this, [this]() { showMergeCandidatesByTypeDialog(); });
    connect(applyMergeAction_, &QAction::triggered, this, [this]() { applyMerge(); });
    connect(validateAction_, &QAction::triggered, this, [this]() { validateShape(); });
    connect(resetViewAction_, &QAction::triggered, this, [this]() { resetView(); });
    connect(fitAllAction_, &QAction::triggered, viewer_, &OccViewWidget::fitAll);
    connect(toggleFeaturesAction_, &QAction::toggled, viewer_, &OccViewWidget::setFeatureLinesVisible);
    connect(undoAction_, &QAction::triggered, this, [this]() { undo(); });
    connect(redoAction_, &QAction::triggered, this, [this]() { redo(); });
}

void MainWindow::openStepFile() {
    if (stlCropInProgress_ || patchApplyInProgress_) {
        inspectPanel_->showReport("后台任务正在运行，请等待当前任务完成。");
        setStatus("后台任务运行中");
        return;
    }

    const auto filePath = QFileDialog::getOpenFileName(
        this, "打开 STEP/STP", QString(), "STEP 文件 (*.step *.stp *.STEP *.STP);;所有文件 (*.*)");
    if (filePath.isEmpty()) {
        return;
    }

    const auto path = pathFromQString(filePath);
    ProcessStatusSnapshot loadingStatus = makeProcessStatus(ProcessStage::LoadingStep, "STEP/STP loading started.");
    loadingStatus.latestMessage = "Loading STEP/STP in background.";
    controller_.updateProcessStatus(loadingStatus);
    setStlCropInProgress(true);
    startPatchPreviewProgressReport(
        loadingStatus,
        QString("STEP/STP 正在后台打开\n文件：%1\n刷新策略：读取完成后回到 GUI 线程刷新 viewer / model tree / report。")
            .arg(filePath));
    logPanel_->appendInfo(QString("开始后台打开 STEP/STP：%1").arg(filePath));
    setStatus("STEP/STP 正在后台打开");

    auto* watcher = new QFutureWatcher<OpenStepUiResult>(this);
    connect(watcher, &QFutureWatcher<OpenStepUiResult>::finished, this, [this, watcher, filePath]() {
        const auto result = watcher->result();
        watcher->deleteLater();
        setStlCropInProgress(false);

        if (!result.success) {
            auto failedStatus = makeProcessStatus(ProcessStage::LoadingStep, result.message);
            failedStatus.latestWarning = result.message;
            appendPatchPreviewProgress(std::move(failedStatus));
            stopPatchPreviewProgressReport();
            QMessageBox::critical(this, "打开 STEP/STP 失败", QString::fromStdString(result.message));
            logPanel_->appendError(QString("打开失败：%1").arg(QString::fromStdString(result.message)));
            setStatus("STEP/STP 打开失败");
            refreshUndoRedoActions();
            refreshPatchApplyAction();
            refreshProcessStatusPanel();
            return;
        }

        auto loadedStatus = makeProcessStatus(ProcessStage::Idle, "STEP/STP loaded.");
        appendPatchPreviewProgress(loadedStatus, false);
        stopPatchPreviewProgressReport();

        const auto& document = controller_.document();
        const auto& stats = document.stats();
        clearMergeCandidateState();
        hasFeatureEdgeResult_ = false;
        refreshModelTree();
        const auto displayStatus = viewer_->displayDocument(document);
        if (!displayStatus.success()) {
            QMessageBox::critical(this, "显示模型失败", QString::fromStdString(displayStatus.message()));
            logPanel_->appendError(QString("显示模型失败：%1").arg(QString::fromStdString(displayStatus.message())));
            setStatus("模型已读取，但显示失败");
            refreshUndoRedoActions();
            refreshPatchApplyAction();
            refreshProcessStatusPanel();
            return;
        }
        showSourceStlAction_->setChecked(true);
        showSourceStlAction_->setEnabled(false);
        showCroppedStlAction_->setChecked(true);
        showCroppedStlAction_->setEnabled(false);
        showStlCropBoxAction_->setChecked(false);
        showStlCropBoxAction_->setEnabled(false);
        syncLockedEdges();
        QTimer::singleShot(0, viewer_, &OccViewWidget::fitAll);
        QTimer::singleShot(100, viewer_, &OccViewWidget::fitAll);
        inspectPanel_->showProperties("模型摘要", {
            {"文件", pathToQString(document.sourcePath())},
            {"实体数", QString::number(stats.solids)},
            {"壳数", QString::number(stats.shells)},
            {"面数", QString::number(stats.faces)},
            {"边数", QString::number(stats.edges)},
            {"顶点数", QString::number(stats.vertices)}
        });
        inspectPanel_->showReport(QString("输入文件：%1\n实体数：%2\n壳数：%3\n面数：%4\n边数：%5")
            .arg(pathToQString(document.sourcePath()))
            .arg(stats.solids)
            .arg(stats.shells)
            .arg(stats.faces)
            .arg(stats.edges));
        logPanel_->appendInfo(QString("已打开 STEP/STP：%1").arg(filePath));
        setStatus("STEP/STP 已加载");
        refreshUndoRedoActions();
        refreshPatchApplyAction();
        refreshProcessStatusPanel();
    });
    watcher->setFuture(QtConcurrent::run([this, path]() {
        const auto result = controller_.openStepFile(path);
        OpenStepUiResult output;
        output.success = result.success();
        output.message = result.message();
        output.path = path;
        return output;
    }));
}

void MainWindow::saveProject() {
    logPanel_->appendWarning("项目保存功能尚未实现。");
    setStatus("项目保存待实现");
}

void MainWindow::exportStepFile() {
    if (stlCropInProgress_ || patchApplyInProgress_) {
        inspectPanel_->showReport("后台任务正在运行，请等待当前任务完成。");
        setStatus("后台任务运行中");
        return;
    }

    if (!controller_.hasDocument()) {
        QMessageBox::information(this, "导出 STEP", "请先打开 STEP/STP 文件再导出。");
        return;
    }

    const auto filePath = QFileDialog::getSaveFileName(
        this, "导出 STEP", QString(), "STEP 文件 (*.step *.stp);;所有文件 (*.*)");
    if (filePath.isEmpty()) {
        return;
    }

    const auto path = pathFromQString(filePath);
    ProcessStatusSnapshot exportStatus = makeProcessStatus(ProcessStage::ExportingStep, "Exporting STEP in background.");
    exportStatus.patchStepPath = path;
    controller_.updateProcessStatus(exportStatus);
    setStlCropInProgress(true);
    startPatchPreviewProgressReport(
        exportStatus,
        QString("STEP 正在后台导出\n文件：%1\n刷新策略：导出和二次读取校验在后台运行，完成后回到 GUI 线程更新 report / status。")
            .arg(filePath));
    logPanel_->appendInfo(QString("开始后台导出 STEP：%1").arg(filePath));
    setStatus("STEP 正在后台导出");

    auto* watcher = new QFutureWatcher<ExportStepUiResult>(this);
    connect(watcher, &QFutureWatcher<ExportStepUiResult>::finished, this, [this, watcher, filePath]() {
        const auto result = watcher->result();
        watcher->deleteLater();
        setStlCropInProgress(false);

        if (!result.exportSuccess) {
            auto failedStatus = makeProcessStatus(ProcessStage::ExportingStep, result.exportMessage);
            failedStatus.patchStepPath = result.path;
            failedStatus.latestWarning = result.exportMessage;
            appendPatchPreviewProgress(std::move(failedStatus));
            stopPatchPreviewProgressReport();
            QMessageBox::critical(this, "导出 STEP 失败", QString::fromStdString(result.exportMessage));
            logPanel_->appendError(QString("导出失败：%1").arg(QString::fromStdString(result.exportMessage)));
            setStatus("STEP 导出失败");
            refreshUndoRedoActions();
            refreshProcessStatusPanel();
            return;
        }

        if (!result.verifySuccess) {
            auto verifyFailedStatus = makeProcessStatus(ProcessStage::ExportingStep, result.verifyMessage);
            verifyFailedStatus.patchStepPath = result.path;
            verifyFailedStatus.latestWarning = result.verifyMessage;
            appendPatchPreviewProgress(std::move(verifyFailedStatus));
            stopPatchPreviewProgressReport();
            logPanel_->appendError(QString("STEP 已导出，但重新读取校验失败：%1").arg(QString::fromStdString(result.verifyMessage)));
            inspectPanel_->showValidation(QString("导出后二次读取校验失败\n文件：%1\n错误：%2")
                .arg(filePath)
                .arg(QString::fromStdString(result.verifyMessage)));
            setStatus("STEP 已导出，二次读取失败");
            refreshUndoRedoActions();
            refreshProcessStatusPanel();
            return;
        }

        auto completedStatus = makeProcessStatus(ProcessStage::Idle, "STEP export completed and readback validation passed.");
        completedStatus.patchStepPath = result.path;
        appendPatchPreviewProgress(std::move(completedStatus));
        stopPatchPreviewProgressReport();
        logPanel_->appendInfo(QString("已导出 STEP 并通过二次读取校验：%1").arg(filePath));
        inspectPanel_->showValidation(QString("导出后二次读取校验通过\n文件：%1").arg(filePath));
        setStatus("STEP 已导出并通过校验");
        refreshUndoRedoActions();
        refreshProcessStatusPanel();
    });
    watcher->setFuture(QtConcurrent::run([this, path]() {
        ExportStepUiResult output;
        output.path = path;

        const auto exportResult = controller_.exportStepFile(path);
        output.exportSuccess = exportResult.success();
        output.exportMessage = exportResult.message();
        if (!output.exportSuccess) {
            return output;
        }

        const auto verifyResult = controller_.verifyStepFileReadable(path);
        output.verifySuccess = verifyResult.success();
        output.verifyMessage = verifyResult.message();
        return output;
    }));
}

void MainWindow::openSourceStlFile() {
    if (!controller_.hasDocument()) {
        QMessageBox::information(this, "打开原始 STL", "请先打开对应的 STEP/STP 文件。");
        setStatus("未加载模型");
        return;
    }

    const auto filePath = QFileDialog::getOpenFileName(
        this, "打开原始 STL", QString(), "STL 文件 (*.stl *.STL);;所有文件 (*.*)");
    if (filePath.isEmpty()) {
        return;
    }

    const auto result = controller_.openStlFile(pathFromQString(filePath));
    if (!result.success()) {
        const auto message = QString::fromStdString(result.message());
        QMessageBox::critical(this, "打开 STL 失败", message);
        logPanel_->appendError(QString("打开 STL 失败：%1").arg(message));
        setStatus("打开 STL 失败");
        return;
    }

    const auto bbox = controller_.sourceStlBoundingBox();
    showSourceStlAction_->setChecked(true);
    showSourceStlAction_->setEnabled(true);
    showCroppedStlAction_->setChecked(true);
    showCroppedStlAction_->setEnabled(false);
    showStlCropBoxAction_->setChecked(false);
    showStlCropBoxAction_->setEnabled(false);
    viewer_->showSourceStl(controller_.sourceStlMesh());
    viewer_->clearCroppedStl();
    viewer_->clearStlCropBox();

    inspectPanel_->showReport(QString("原始 STL 已加载\n文件：%1\ntriangle count：%2\nviewer displayed triangles：%3\nbbox：%4")
        .arg(filePath)
        .arg(controller_.sourceStlTriangleCount())
        .arg(viewer_->sourceStlDisplayedTriangleCount())
        .arg(stlBoundingBoxText(bbox)));
    bottomTabs_->setCurrentWidget(inspectPanel_->reportWidget());
    logPanel_->appendInfo(QString("已打开原始 STL：%1，triangle %2")
        .arg(filePath)
        .arg(controller_.sourceStlTriangleCount()));
    setStatus("原始 STL 已加载");
    refreshProcessStatusPanel();
}

void MainWindow::cropCurrentCandidateStl() {
    if (stlCropInProgress_) {
        inspectPanel_->showReport("STL 裁剪正在后台运行，请等待当前任务完成。");
        setStatus("STL 裁剪进行中");
        return;
    }

    if (!controller_.hasDocument()) {
        inspectPanel_->showReport("请先打开 STEP/STP 文件。");
        setStatus("未加载模型");
        return;
    }

    auto* candidate = currentMergeCandidate();
    if (candidate == nullptr) {
        inspectPanel_->showReport("请先在候选选择模式下点击一个候选区域，或按 ID 高亮一个候选区域。");
        logPanel_->appendWarning("裁剪 STL 前未选择候选区域。");
        setStatus("未选择候选区域");
        return;
    }

    if (candidate->candidate_type != MergeCandidateType::FeatureBoundedRefit) {
        inspectPanel_->showReport(QString("当前候选不是 FeatureBoundedRefit，不能执行 STL 裁剪。\n候选 ID：%1\n候选类型：%2")
            .arg(candidate->candidate_id)
            .arg(candidateTypeText(candidate->candidate_type)));
        setStatus("当前候选不能裁剪 STL");
        return;
    }

    if (!controller_.hasSourceStl()) {
        inspectPanel_->showReport("请先通过 STL -> 打开原始 STL 加载源 STL。");
        logPanel_->appendWarning("裁剪 STL 前未加载源 STL。");
        setStatus("未加载源 STL");
        return;
    }

    auto documentStem = pathToQString(controller_.document().sourcePath().stem());
    if (documentStem.isEmpty()) {
        documentStem = "document";
    }
    const auto defaultName = QString("%1_candidate_%2.stl")
        .arg(documentStem)
        .arg(candidate->candidate_id, 4, 10, QLatin1Char('0'));
    const auto defaultPath = std::filesystem::temp_directory_path() /
        std::filesystem::path(defaultName.toStdWString());
    const auto filePath = QFileDialog::getSaveFileName(
        this, "写出 local STL", pathToQString(defaultPath), "STL 文件 (*.stl);;所有文件 (*.*)");
    if (filePath.isEmpty()) {
        return;
    }

    const auto candidateSnapshot = *candidate;
    const auto outputPath = pathFromQString(filePath);
    const auto sourceStlPath = controller_.sourceStlPath();
    const auto documentSnapshot = controller_.document();
    const auto sourceMeshSnapshot = controller_.sourceStlMesh();
    const auto cropOptions = currentStlCropOptions();
    const auto cropMode = cropOptions.mode == StlCropMode::ConservativeBoundaryBand
        ? QString("conservative-boundary-band")
        : QString("centroid-only");

    ProcessStatusSnapshot cropStatus = makeProcessStatus(
        ProcessStage::CroppingStl,
        QString("STL crop started for current candidate. mode=%1").arg(cropMode).toStdString());
    cropStatus.candidateId = candidateSnapshot.candidate_id;
    cropStatus.sourceFaceCount = candidateSnapshot.face_count;
    cropStatus.boundaryEdgeCount = candidateSnapshot.boundary_edge_count;
    cropStatus.localStlPath = outputPath;
    controller_.updateProcessStatus(cropStatus);
    refreshProcessStatusPanel();

    setStlCropInProgress(true);
    inspectPanel_->showReport(QString("STL 裁剪正在后台运行\nsource STL：%1\noutput STL：%2\ncandidate id：%3\ncandidate type：%4\ncrop mode：%5")
        .arg(pathToQString(sourceStlPath))
        .arg(filePath)
        .arg(candidateSnapshot.candidate_id)
        .arg(candidateTypeText(candidateSnapshot.candidate_type))
        .arg(cropMode));
    bottomTabs_->setCurrentWidget(inspectPanel_->reportWidget());
    logPanel_->appendInfo(QString("开始后台裁剪 STL：候选 %1，输出 %2")
        .arg(candidateSnapshot.candidate_id)
        .arg(filePath));
    setStatus("STL 裁剪进行中");

    auto* watcher = new QFutureWatcher<StlCandidateCropResult>(this);
    connect(watcher, &QFutureWatcher<StlCandidateCropResult>::finished, this, [this, watcher, candidateSnapshot, filePath, sourceStlPath, cropMode]() {
        const auto result = watcher->result();
        watcher->deleteLater();
        setStlCropInProgress(false);

        const auto& report = result.extract.report;
        if (!result.success) {
            ProcessStatusSnapshot cropFailed = makeProcessStatus(ProcessStage::CroppingStl, result.message);
            cropFailed.candidateId = candidateSnapshot.candidate_id;
            cropFailed.sourceFaceCount = candidateSnapshot.face_count;
            cropFailed.boundaryEdgeCount = candidateSnapshot.boundary_edge_count;
            cropFailed.localStlPath = pathFromQString(filePath);
            cropFailed.latestWarning = result.message;
            controller_.updateProcessStatus(cropFailed);
            refreshProcessStatusPanel();
            const auto message = QString::fromStdString(result.message);
            inspectPanel_->showReport(QString("STL 裁剪失败\n候选 ID：%1\n候选类型：%2\n消息：%3")
                .arg(candidateSnapshot.candidate_id)
                .arg(candidateTypeText(candidateSnapshot.candidate_type))
                .arg(message));
            logPanel_->appendWarning(QString("STL 裁剪失败：候选 %1，%2")
                .arg(candidateSnapshot.candidate_id)
                .arg(message));
            setStatus("STL 裁剪失败");
            return;
        }

        showCroppedStlAction_->setChecked(true);
        showCroppedStlAction_->setEnabled(true);
        showStlCropBoxAction_->setChecked(false);
        showStlCropBoxAction_->setEnabled(true);
        viewer_->showCroppedStl(result.extract.localMesh);
        viewer_->showStlCropBox(report.expanded_bbox);
        ProcessStatusSnapshot cropDone = makeProcessStatus(ProcessStage::CroppingStl, "STL crop completed.");
        cropDone.candidateId = candidateSnapshot.candidate_id;
        cropDone.sourceFaceCount = candidateSnapshot.face_count;
        cropDone.boundaryEdgeCount = candidateSnapshot.boundary_edge_count;
        cropDone.localStlPath = result.outputPath;
        controller_.updateProcessStatus(cropDone);
        refreshProcessStatusPanel();

        const auto cropWarning = report.warning_message.empty()
            ? QString()
            : QString::fromStdString(report.warning_message);
        QString cropReport = QString("STL 裁剪完成  |  候选 %1  |  输出 %2 triangles（源 %3）|  模式 %4")
            .arg(candidateSnapshot.candidate_id)
            .arg(report.output_triangle_count)
            .arg(report.source_triangle_count)
            .arg(cropMode);
        if (!cropWarning.isEmpty()) {
            cropReport += QString("\n警告：%1").arg(cropWarning);
        }
        inspectPanel_->showReport(cropReport);
        bottomTabs_->setCurrentWidget(inspectPanel_->reportWidget());
        setStatus("STL 裁剪完成");
    });
    watcher->setFuture(QtConcurrent::run([documentSnapshot, sourceMeshSnapshot, candidateSnapshot, outputPath, cropOptions]() {
        return AppController::cropStlForCandidateData(
            documentSnapshot,
            sourceMeshSnapshot,
            candidateSnapshot,
            outputPath,
            cropOptions);
    }));
}

void MainWindow::generateAndPreviewCurrentPatch() {
    if (stlCropInProgress_) {
        inspectPanel_->showReport("STL 裁剪或 Patch 生成正在后台运行，请等待当前任务完成。");
        setStatus("Patch 预览生成中");
        return;
    }

    if (!controller_.hasDocument()) {
        inspectPanel_->showReport("请先打开 STEP/STP 文件。");
        setStatus("未加载模型");
        return;
    }

    auto* candidate = currentMergeCandidate();
    if (candidate == nullptr) {
        inspectPanel_->showReport("请先在候选选择模式下点击一个候选区域，或按 ID 高亮一个候选区域。");
        logPanel_->appendWarning("生成 Patch 前未选择候选区域。");
        setStatus("未选择候选区域");
        return;
    }

    if (candidate->candidate_type != MergeCandidateType::FeatureBoundedRefit) {
        inspectPanel_->showReport(QString("当前候选不是 FeatureBoundedRefit，不能生成 Geomagic patch。\n候选 ID：%1\n候选类型：%2")
            .arg(candidate->candidate_id)
            .arg(candidateTypeText(candidate->candidate_type)));
        setStatus("当前候选不能生成 Patch");
        return;
    }

    const auto fittingInputMode = currentFittingInputMode();
    const auto needsSourceStl = fittingInputMode != GeomagicFittingInputMode::StpSampledCandidateSurface;
    if (needsSourceStl && !controller_.hasSourceStl()) {
        inspectPanel_->showReport("请先通过 STL -> 打开原始 STL 加载源 STL。");
        logPanel_->appendWarning("生成 Patch 前未加载源 STL。");
        setStatus("未加载源 STL");
        return;
    }

    const auto candidateSnapshot = *candidate;
    const auto sourceStlPath = controller_.sourceStlPath();
    const auto documentSnapshot = controller_.document();
    const auto sourceMeshSnapshot = controller_.sourceStlMesh();
    const auto workspaceRoot = std::filesystem::current_path();
    const auto cropOptions = currentStlCropOptions();
    const auto fittingModeStr = QString::fromStdString(toString(fittingInputMode));
    const bool useGeomagicRemesh = useGeomagicRemeshAction_ != nullptr && useGeomagicRemeshAction_->isChecked();
    const auto geomagicRemeshMode = useGeomagicRemesh ? QString("enabled") : QString("disabled");
    const auto runLogger = PatchPreviewRunLogger::create(workspaceRoot, candidateSnapshot.candidate_id);
    runLogger.log(
        "Start",
        "Patch preview requested: fitting_mode=" + fittingModeStr.toStdString() +
            ", geomagic_remesh=" + geomagicRemeshMode.toStdString());
    if (runLogger.ready()) {
        logPanel_->appendInfo(QString("Patch preview run log：%1").arg(pathToQString(runLogger.path())));
    } else {
        logPanel_->appendWarning(QString("Patch preview run log 创建失败：%1")
            .arg(QString::fromStdString(runLogger.errorMessage())));
    }

    ProcessStatusSnapshot pipelineStatus = makeProcessStatus(
        ProcessStage::AnalyzingBoundary,
        QString("Patch preview pipeline started: fitting_mode=%1, geomagic_remesh=%2")
            .arg(fittingModeStr, geomagicRemeshMode)
            .toStdString());
    pipelineStatus.candidateId = candidateSnapshot.candidate_id;
    pipelineStatus.sourceFaceCount = candidateSnapshot.face_count;
    pipelineStatus.boundaryEdgeCount = candidateSnapshot.boundary_edge_count;
    pipelineStatus.patchPreviewRunLogPath = runLogger.path();

    setStlCropInProgress(true);
    startPatchPreviewProgressReport(
        pipelineStatus,
        QString("Patch 预览链路正在后台运行\nsource STL：%1\ncandidate id：%2\ncandidate type：%3\nfitting input mode：%4\nGeomagic Remesh：%5\nrun log：%6\n刷新策略：阶段事件立即追加；长阶段每 2 秒刷新心跳行。")
        .arg(pathToQString(sourceStlPath))
        .arg(candidateSnapshot.candidate_id)
        .arg(candidateTypeText(candidateSnapshot.candidate_type))
        .arg(fittingModeStr)
        .arg(geomagicRemeshMode)
        .arg(pathToQString(runLogger.path())));
    logPanel_->appendInfo(QString("开始生成 Patch 预览：候选 %1").arg(candidateSnapshot.candidate_id));
    setStatus("Patch 预览生成中");

    QPointer<MainWindow> window(this);
    auto progressCallback = [window](ProcessStatusSnapshot status) mutable {
        if (window.isNull()) {
            return;
        }
        QMetaObject::invokeMethod(window.data(), [window, status = std::move(status)]() mutable {
            if (window.isNull()) {
                return;
            }
            window->appendPatchPreviewProgress(std::move(status));
        }, Qt::QueuedConnection);
    };

    auto* watcher = new QFutureWatcher<PatchPreviewPipelineResult>(this);
    connect(watcher, &QFutureWatcher<PatchPreviewPipelineResult>::finished, this, [this, watcher, candidateSnapshot, sourceStlPath, fittingModeStr, geomagicRemeshMode, runLogger]() {
        const auto result = watcher->result();
        watcher->deleteLater();
        setStlCropInProgress(false);
        runLogger.log(
            "WorkerFinished",
            result.success
                ? "Patch preview worker completed successfully."
                : "Patch preview worker failed: " + result.message);

        if (!result.success) {
            viewer_->clearPatchOverlay();
            controller_.clearCurrentPatchOverlay();
            ProcessStatusSnapshot failedStatus = makeProcessStatus(
                result.crop.success ? ProcessStage::RunningGeomagic : ProcessStage::CroppingStl,
                result.message);
            failedStatus.candidateId = candidateSnapshot.candidate_id;
            failedStatus.sourceFaceCount = candidateSnapshot.face_count;
            failedStatus.boundaryEdgeCount = candidateSnapshot.boundary_edge_count;
            failedStatus.localStlPath = result.crop.outputPath;
            failedStatus.patchStepPath = result.geomagic.outputStepPath;
            failedStatus.patchIgesPath = result.geomagic.outputIgesPath;
            failedStatus.fitRegionLogPath = result.geomagic.fitRegionLogPath;
            failedStatus.patchPreviewRunLogPath = result.patchPreviewRunLogPath.empty()
                ? runLogger.path()
                : result.patchPreviewRunLogPath;
            failedStatus.latestWarning = result.message;
            appendPatchPreviewProgress(std::move(failedStatus));
            stopPatchPreviewProgressReport();
            const auto message = QString::fromStdString(result.message);
            inspectPanel_->showReport(QString("Patch 预览链路失败\nsource STL：%1\ncandidate id：%2\nfitting input mode：%3\nGeomagic Remesh：%4\nlocal STL：%5\noutput STEP：%6\nfit_region log：%7\nrun log：%8\n消息：%9")
                .arg(pathToQString(sourceStlPath))
                .arg(candidateSnapshot.candidate_id)
                .arg(fittingModeStr)
                .arg(geomagicRemeshMode)
                .arg(pathToQString(result.crop.outputPath))
                .arg(pathToQString(result.geomagic.outputStepPath))
                .arg(pathToQString(result.geomagic.fitRegionLogPath))
                .arg(pathToQString(failedStatus.patchPreviewRunLogPath))
                .arg(message));
            bottomTabs_->setCurrentWidget(inspectPanel_->reportWidget());
            logPanel_->appendWarning(QString("Patch 预览链路失败：候选 %1，%2")
                .arg(candidateSnapshot.candidate_id)
                .arg(message));
            setStatus("Patch 预览生成失败");
            refreshPatchApplyAction();
            return;
        }

        showCroppedStlAction_->setChecked(true);
        showCroppedStlAction_->setEnabled(true);
        showStlCropBoxAction_->setChecked(false);
        showStlCropBoxAction_->setEnabled(true);
        viewer_->showCroppedStl(result.crop.extract.localMesh);
        viewer_->showStlCropBox(result.crop.extract.report.expanded_bbox);

        ProcessStatusSnapshot importingStatus = makeProcessStatus(ProcessStage::ImportingPatch, "Importing Geomagic patch result.");
        importingStatus.candidateId = candidateSnapshot.candidate_id;
        importingStatus.sourceFaceCount = candidateSnapshot.face_count;
        importingStatus.boundaryEdgeCount = candidateSnapshot.boundary_edge_count;
        importingStatus.localStlPath = result.crop.outputPath;
        importingStatus.patchStepPath = result.geomagic.outputStepPath;
        importingStatus.patchIgesPath = result.geomagic.outputIgesPath;
        importingStatus.fitRegionLogPath = result.geomagic.fitRegionLogPath;
        importingStatus.patchPreviewRunLogPath = result.patchPreviewRunLogPath.empty()
            ? runLogger.path()
            : result.patchPreviewRunLogPath;
        appendPatchPreviewProgress(importingStatus);

        runLogger.log("ImportingPatch", "Importing Geomagic patch result started.");
        const auto importStart = std::chrono::steady_clock::now();
        const auto import = controller_.importPatchResultForCurrentCandidate(result.geomagic, &candidateSnapshot);
        if (!import.success()) {
            runLogger.logDuration(
                "ImportingPatch",
                "Patch import failed: " + import.message(),
                importStart);
            viewer_->clearPatchOverlay();
            const auto message = QString::fromStdString(import.message());
            auto importFailed = importingStatus;
            importFailed.latestMessage = import.message();
            importFailed.latestWarning = import.message();
            appendPatchPreviewProgress(std::move(importFailed));
            stopPatchPreviewProgressReport();
            inspectPanel_->showReport(QString("Patch 导入失败\nlocal STL：%1\noutput STEP：%2\nrun log：%3\n错误：%4")
                .arg(pathToQString(result.crop.outputPath))
                .arg(pathToQString(result.geomagic.outputStepPath))
                .arg(pathToQString(importingStatus.patchPreviewRunLogPath))
                .arg(message));
            bottomTabs_->setCurrentWidget(inspectPanel_->reportWidget());
            logPanel_->appendWarning(QString("Patch 导入失败：候选 %1，%2")
                .arg(candidateSnapshot.candidate_id)
                .arg(message));
            setStatus("Patch 导入失败");
            refreshPatchApplyAction();
            return;
        }
        runLogger.logDuration("ImportingPatch", "Patch import finished.", importStart);

        const auto previewStart = std::chrono::steady_clock::now();
        runLogger.log("PreviewReady", "Viewer overlay and crop boundary diagnostics started.");
        viewer_->showPatchCutoutPreview(candidateSnapshot.faces);
        viewer_->showPatchOverlay(controller_.currentImportedPatchInfo().shape);
        const auto diagnostics = controller_.diagnoseCropBoundaryForCurrentPatch(
            candidateSnapshot,
            &result.crop.extract.localMesh);
        viewer_->showCropBoundaryDiagnosticsOverlay(diagnostics);
        publishCropBoundaryDiagnosticsStatus(diagnostics);
        appendPatchPreviewProgress(controller_.currentProcessStatus());
        stopPatchPreviewProgressReport();
        showPatchPreviewReport(controller_.currentPatchPreviewReport(), true, &diagnostics, fittingModeStr);
        refreshPatchApplyAction();
        refreshProcessStatusPanel();
        setStatus("Patch cutout overlay 已显示");
        runLogger.logDuration("PreviewReady", "Viewer overlay and crop boundary diagnostics finished.", previewStart);
        runLogger.log("Finished", "Patch preview ready.");
    });
    watcher->setFuture(QtConcurrent::run([documentSnapshot, sourceMeshSnapshot, candidateSnapshot, workspaceRoot, cropOptions, fittingInputMode, useGeomagicRemesh, progressCallback, runLogger]() {
        GeomagicAutoSurfaceConfig config;
        config.strictPatchTarget = false;
        config.skipRemesh = !useGeomagicRemesh;
        return AppController::generateFittingStlAndRunGeomagicForCandidateData(
            documentSnapshot,
            sourceMeshSnapshot,
            candidateSnapshot,
            workspaceRoot,
            config,
            fittingInputMode,
            cropOptions,
            {},
            progressCallback,
            runLogger);
    }));
}

void MainWindow::importPatchForCurrentCandidate() {
    auto* candidate = currentMergeCandidate();
    if (candidate == nullptr) {
        inspectPanel_->showReport("请先选择一个候选区域，再导入当前候选对应的 Geomagic patch。");
        logPanel_->appendWarning("导入当前候选 Patch 前未选择候选区域。");
        setStatus("未选择候选区域");
        return;
    }

    const auto defaultRoot = std::filesystem::current_path() / "data" / "crop_stl";
    const auto filePath = QFileDialog::getOpenFileName(
        this,
        "选择当前候选 local STL（用于定位同名 Patch）",
        pathToQString(defaultRoot),
        "STL 文件 (*.stl *.STL);;所有文件 (*.*)");
    if (filePath.isEmpty()) {
        return;
    }

    const auto result = controller_.importPatchForCurrentCandidateFromLocalStl(pathFromQString(filePath), candidate);
    if (!result.success()) {
        viewer_->clearPatchOverlay();
        refreshProcessStatusPanel();
        const auto message = QString::fromStdString(result.message());
        inspectPanel_->showReport(QString("Patch 导入失败\nlocal STL：%1\n错误：%2")
            .arg(filePath)
            .arg(message));
        logPanel_->appendWarning(QString("Patch 导入失败：%1").arg(message));
        setStatus("Patch 导入失败");
        refreshPatchApplyAction();
        return;
    }

    viewer_->showPatchOverlay(controller_.currentImportedPatchInfo().shape);
    const auto localStl = StlReader().read(pathFromQString(filePath));
    const StlMesh* localStlMesh = localStl.success ? &localStl.mesh : nullptr;
    auto diagnostics = controller_.diagnoseCropBoundaryForCurrentPatch(*candidate, localStlMesh);
    if (!localStl.success) {
        diagnostics.warningMessage = diagnostics.warningMessage.empty()
            ? localStl.message
            : diagnostics.warningMessage + " " + localStl.message;
    }
    viewer_->showCropBoundaryDiagnosticsOverlay(diagnostics);
    showPatchPreviewReport(controller_.currentPatchPreviewReport(), false, &diagnostics);
    publishCropBoundaryDiagnosticsStatus(diagnostics);
    refreshPatchApplyAction();
    refreshProcessStatusPanel();
    logPanel_->appendInfo(QString("Patch overlay 已导入：%1")
        .arg(pathToQString(controller_.currentPatchPreviewReport().patchStepPath)));
    setStatus("Patch overlay 已显示");
}

void MainWindow::importPatchFromFile() {
    const auto filePath = QFileDialog::getOpenFileName(
        this,
        "选择 Geomagic Patch",
        pathToQString(std::filesystem::current_path()),
        "CAD Patch (*.stp *.step *.igs *.iges *.STP *.STEP *.IGS *.IGES);;所有文件 (*.*)");
    if (filePath.isEmpty()) {
        return;
    }

    const auto result = controller_.importPatchFromFileForCurrentCandidate(pathFromQString(filePath), currentMergeCandidate());
    if (!result.success()) {
        viewer_->clearPatchOverlay();
        refreshProcessStatusPanel();
        const auto message = QString::fromStdString(result.message());
        inspectPanel_->showReport(QString("Patch 文件导入失败\npatch：%1\n错误：%2")
            .arg(filePath)
            .arg(message));
        logPanel_->appendWarning(QString("Patch 文件导入失败：%1").arg(message));
        setStatus("Patch 导入失败");
        refreshPatchApplyAction();
        return;
    }

    viewer_->showPatchOverlay(controller_.currentImportedPatchInfo().shape);
    const auto* candidate = currentMergeCandidate();
    const auto diagnostics = candidate != nullptr
        ? controller_.diagnoseCropBoundaryForCurrentPatch(*candidate, nullptr)
        : CropBoundaryDiagnosticsReport {};
    viewer_->showCropBoundaryDiagnosticsOverlay(diagnostics);
    showPatchPreviewReport(controller_.currentPatchPreviewReport(), false, candidate != nullptr ? &diagnostics : nullptr);
    if (candidate != nullptr) {
        publishCropBoundaryDiagnosticsStatus(diagnostics);
    }
    refreshPatchApplyAction();
    refreshProcessStatusPanel();
    logPanel_->appendInfo(QString("Patch overlay 已从文件导入：%1").arg(filePath));
    setStatus("Patch overlay 已显示");
}

void MainWindow::applyCurrentPatchPreview() {
    if (patchApplyInProgress_) {
        inspectPanel_->showReport("Patch Apply 正在后台运行，请等待当前任务结束。");
        bottomTabs_->setCurrentWidget(inspectPanel_->reportWidget());
        setStatus("Patch Apply 正在运行");
        return;
    }

    auto* candidate = currentMergeCandidate();
    if (candidate == nullptr) {
        controller_.updateProcessStatus(makeProcessStatus(ProcessStage::ApplyFailed, "Patch Apply requires a selected candidate."));
        refreshProcessStatusPanel();
        inspectPanel_->showReport("请先选择候选区域。");
        logPanel_->appendWarning("Patch Apply 被阻止：未选择候选区域。");
        setStatus("Patch Apply 被阻止");
        refreshPatchApplyAction();
        return;
    }

    const auto candidateSnapshot = *candidate;
    ProcessStatusSnapshot applyingStatus = makeProcessStatus(ProcessStage::ApplyingPatch, "Patch Apply started.");
    applyingStatus.candidateId = candidateSnapshot.candidate_id;
    applyingStatus.sourceFaceCount = candidateSnapshot.face_count;
    applyingStatus.boundaryEdgeCount = candidateSnapshot.boundary_edge_count;
    applyingStatus.localStlPath = controller_.currentPatchArtifactPaths().localStlPath;
    applyingStatus.patchStepPath = controller_.currentPatchArtifactPaths().patchStepPath;
    applyingStatus.patchIgesPath = controller_.currentPatchArtifactPaths().patchIgesSidecarPath;
    applyingStatus.fitRegionLogPath = controller_.currentPatchArtifactPaths().fitRegionLogPath;
    controller_.updateProcessStatus(applyingStatus);
    refreshProcessStatusPanel();

    patchApplyInProgress_ = true;
    setStlCropInProgress(true);
    startPatchPreviewProgressReport(
        applyingStatus,
        QString("Patch Apply 正在后台运行\ncandidate id：%1\ncandidate type：%2\n刷新策略：阶段事件立即追加；长阶段每 2 秒刷新心跳行。")
            .arg(candidateSnapshot.candidate_id)
            .arg(candidateTypeText(candidateSnapshot.candidate_type)));
    logPanel_->appendInfo(QString("开始 Patch Apply：候选 %1").arg(candidateSnapshot.candidate_id));
    setStatus("Patch Apply 运行中");

    QPointer<MainWindow> window(this);
    auto progressCallback = [window](ProcessStatusSnapshot status) mutable {
        if (window.isNull()) {
            return;
        }
        QMetaObject::invokeMethod(window.data(), [window, status = std::move(status)]() mutable {
            if (window.isNull()) {
                return;
            }
            window->appendPatchPreviewProgress(std::move(status), false);
        }, Qt::QueuedConnection);
    };

    auto* watcher = new QFutureWatcher<PatchApplyUiResult>(this);
    connect(watcher, &QFutureWatcher<PatchApplyUiResult>::finished, this, [this, watcher, candidateSnapshot]() {
        const auto output = watcher->result();
        watcher->deleteLater();

        patchApplyInProgress_ = false;
        setStlCropInProgress(false);
        appendPatchPreviewProgress(controller_.currentProcessStatus());
        stopPatchPreviewProgressReport();

        const auto statusMessage = output.statusMessage.empty()
            ? QString::fromStdString(output.resultMessage)
            : QString::fromStdString(output.statusMessage);
        inspectPanel_->showReport(patchApplyReportText(output.report, statusMessage));
        bottomTabs_->setCurrentWidget(inspectPanel_->reportWidget());
        refreshProcessStatusPanel();

        if (output.success) {
            viewer_->clearPatchOverlay();
            refreshDocumentViews(false);
            refreshUndoRedoActions();
            refreshPatchApplyAction();
            logPanel_->appendInfo(QString("Patch Apply 完成：候选 %1 已通过 StrictTopologyGate 并提交。").arg(output.report.candidateId));
            setStatus("Patch Apply 完成");
        } else {
            refreshUndoRedoActions();
            refreshPatchApplyAction();
            logPanel_->appendWarning(QString("Patch Apply 失败：候选 %1，%2")
                .arg(candidateSnapshot.candidate_id)
                .arg(statusMessage));
            setStatus("Patch Apply 失败");
        }
    });
    watcher->setFuture(QtConcurrent::run([this, candidateSnapshot, progressCallback]() {
        PatchApplyUiResult output;
        const auto result = controller_.applyCurrentPatchToCurrentCandidate(
            candidateSnapshot,
            &output.report,
            progressCallback);
        output.success = result.success();
        output.resultMessage = result.message();
        output.statusMessage = controller_.currentPatchStatusMessage();
        return output;
    }));
}

void MainWindow::clearPatchOverlay() {
    viewer_->clearPatchOverlay();
    controller_.clearCurrentPatchOverlay();
    inspectPanel_->showReport("Patch overlay 已清除。\n主模型未修改。");
    setStatus("Patch overlay 已清除");
    refreshPatchApplyAction();
    refreshProcessStatusPanel();
}

void MainWindow::showPatchPreviewReport(
    const PatchPreviewReport& report,
    bool visualCutoutPreview,
    const CropBoundaryDiagnosticsReport* diagnostics,
    const QString& /*cropMode*/) {
    const auto warning = QString::fromStdString(report.warningMessage);
    const auto message = QString::fromStdString(report.message);

    QString reportText = QString("Patch 预览\n状态：%1  |  候选 %2  |  源面 %3  |  patch 面 %4  |  BRepCheck %5")
        .arg(boolText(report.success))
        .arg(report.candidateId)
        .arg(report.sourceFaceCount)
        .arg(report.patchFaceCount)
        .arg(boolText(report.patchBRepCheckValid));
    if (!warning.isEmpty()) {
        reportText += QString("\n警告：%1").arg(warning);
    }
    if (report.highRisk) {
        reportText += "\n⚠ 高风险，不建议 Apply";
    }
    if (diagnostics != nullptr && diagnostics->suspectedGapCount > 0) {
        reportText += QString("\n边界缺口：%1 段，边 %2")
            .arg(diagnostics->suspectedGapCount)
            .arg(gapEdgeIdsText(diagnostics->suspectedGapEdgeIds));
    }

    inspectPanel_->showReport(reportText);
    bottomTabs_->setCurrentWidget(inspectPanel_->reportWidget());
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
    inspectPanel_->showReport(QString("特征边检测完成  |  %1 条边  |  Sharp %2  |  Free %3  |  Multiple %4")
        .arg(result.edges.size())
        .arg(result.sharp_edges)
        .arg(result.free_edges)
        .arg(result.multiple_edges));
    setStatus("特征边检测完成");
    refreshUndoRedoActions();
}

void MainWindow::previewMergeCandidates() {
    if (stlCropInProgress_ || patchApplyInProgress_) {
        inspectPanel_->showReport("后台任务正在运行，请等待当前任务完成。");
        setStatus("后台任务运行中");
        return;
    }

    if (!controller_.hasDocument()) {
        inspectPanel_->showReport("请先打开 STEP/STP 文件。");
        setStatus("未加载模型");
        return;
    }

    const auto beforeStats = controller_.document().stats();
    const auto params = parameterPanel_->parameters();
    MergePlannerOptions options;
    options.enable_feature_bounded_refit_candidates = true;
    options.min_feature_bounded_region_faces = 2;

    auto progress = makeProcessStatus(
        ProcessStage::PreviewingMergeCandidates,
        "Merge candidate preview started.");
    controller_.updateProcessStatus(progress);
    setStlCropInProgress(true);
    startPatchPreviewProgressReport(
        progress,
        "合并候选区域预览正在后台运行\n刷新策略：候选计算在 worker，viewer / model tree / report 更新回 GUI 线程。");
    logPanel_->appendInfo("开始后台预览合并候选区域。");
    setStatus("合并候选区域预览中");

    auto* watcher = new QFutureWatcher<MergePreviewUiResult>(this);
    connect(watcher, &QFutureWatcher<MergePreviewUiResult>::finished, this, [this, watcher]() {
        auto output = watcher->result();
        watcher->deleteLater();
        setStlCropInProgress(false);

        auto completed = makeProcessStatus(ProcessStage::PreviewingMergeCandidates, "Merge candidate preview completed.");
        completed.latestMessage = "Merge candidate preview completed.";
        appendPatchPreviewProgress(completed);
        stopPatchPreviewProgressReport();

        clearMergeCandidateState();
        lastMergeCandidates_ = std::move(output.result.candidates);
        hasFeatureEdgeResult_ = true;

        int maxFaceCount = 0;
        int totalCandidateFaces = 0;
        for (const auto& candidate : lastMergeCandidates_) {
            maxFaceCount = std::max(maxFaceCount, candidate.face_count);
            totalCandidateFaces += candidate.face_count;
        }
        const auto statusCounts = countCandidateStatuses(lastMergeCandidates_);
        const auto typeCounts = countCandidateTypes(lastMergeCandidates_);

        QString report = QString("合并候选区域预览完成\n候选区域数量：%1\nFeatureBoundedRefit：%2\nUnknown：%3\nPending：%4\nAccepted：%5\nRejected：%6\nHidden：%7\n保护边数量：%8\n访问 face 数量：%9\n拒绝区域数量：%10\n最大候选区域 face 数：%11\n总候选 face 数：%12\n预览前 face/edge：%13/%14\n预览后 face/edge：%15/%16")
            .arg(lastMergeCandidates_.size())
            .arg(typeCounts.feature_bounded_refit)
            .arg(typeCounts.unknown)
            .arg(statusCounts.pending)
            .arg(statusCounts.accepted)
            .arg(statusCounts.rejected)
            .arg(statusCounts.hidden)
            .arg(output.result.protected_edge_count)
            .arg(output.result.visited_faces)
            .arg(output.result.rejected_regions)
            .arg(maxFaceCount)
            .arg(totalCandidateFaces)
            .arg(output.beforeStats.faces)
            .arg(output.beforeStats.edges)
            .arg(output.afterStats.faces)
            .arg(output.afterStats.edges);

        const auto previewCount = std::min<std::size_t>(10, lastMergeCandidates_.size());
        for (std::size_t index = 0; index < previewCount; ++index) {
            const auto& candidate = lastMergeCandidates_[index];
            report += QString("\n\n候选 %1\n状态：%2\n类型：%3\nface 数：%4\n总面积：%5\n最大法向夹角：%6 度\n最大距离：%7\n边界边数：%8\n内部边数：%9\nfit_error：%10\n风险：%11")
                .arg(candidate.candidate_id)
                .arg(candidateStatusText(candidate.status))
                .arg(candidateTypeText(candidate.candidate_type))
                .arg(candidate.face_count)
                .arg(QString::number(candidate.total_area, 'f', 4))
                .arg(QString::number(candidate.max_normal_angle_deg, 'f', 3))
                .arg(QString::number(candidate.max_distance, 'g', 6))
                .arg(candidate.boundary_edge_count)
                .arg(candidate.internal_edge_count)
                .arg(QString::number(candidate.fit_error, 'g', 6))
                .arg(riskLevelText(candidate.risk_level));
        }

        if (lastMergeCandidates_.empty()) {
            viewer_->clearMergeCandidates();
            visibleMergeCandidateCount_ = 0;
            visibleMergeCandidateIds_.clear();
            report += "\n\n没有可预览候选区域。";
        } else {
            viewer_->showMergeCandidates(lastMergeCandidates_, 10, false);
            visibleMergeCandidateCount_ = static_cast<int>(std::min<std::size_t>(10, lastMergeCandidates_.size()));
            visibleMergeCandidateIds_.clear();
            for (std::size_t index = 0; index < static_cast<std::size_t>(visibleMergeCandidateCount_); ++index) {
                addVisibleCandidateId(visibleMergeCandidateIds_, lastMergeCandidates_[index]);
            }
        }
        viewer_->showFeatureEdges(controller_.featureEdges());
        viewer_->setFeatureLinesVisible(true);
        toggleFeaturesAction_->setChecked(true);
        refreshModelTree();
        inspectPanel_->showReport(report);
        logPanel_->appendInfo(QString("合并候选区域预览完成：候选 %1 个，FeatureBoundedRefit %2 个，保护边 %3，访问 face %4，拒绝 %5")
            .arg(lastMergeCandidates_.size())
            .arg(typeCounts.feature_bounded_refit)
            .arg(output.result.protected_edge_count)
            .arg(output.result.visited_faces)
            .arg(output.result.rejected_regions));
        setStatus("合并候选区域预览完成");
        refreshProcessStatusPanel();
    });
    watcher->setFuture(QtConcurrent::run([this, beforeStats, params, options]() {
        MergePreviewUiResult output;
        output.beforeStats = beforeStats;
        output.result = controller_.previewMergeCandidates(
            params.angular_threshold_degrees,
            params.min_edge_length,
            options);
        output.afterStats = controller_.document().stats();
        return output;
    }));
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

    const auto visibleCandidates = filterNonHiddenCandidates(lastMergeCandidates_);

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

void MainWindow::showMergeCandidatesByTypeDialog() {
    if (lastMergeCandidates_.empty()) {
        logPanel_->appendWarning("当前没有候选区域，请先点击“预览合并”。");
        setStatus("没有候选区域");
        return;
    }

    QStringList items;
    for (const auto type : displayCandidateTypes()) {
        items << candidateTypeText(type);
    }

    bool ok = false;
    const auto selected = QInputDialog::getItem(this, "按类型显示候选", "候选类型：", items, 0, false, &ok);
    if (!ok || selected.isEmpty()) {
        return;
    }

    for (const auto type : displayCandidateTypes()) {
        if (candidateTypeText(type) == selected) {
            showMergeCandidatesByType(type);
            return;
        }
    }
}

void MainWindow::showMergeCandidatesByType(MergeCandidateType type) {
    if (lastMergeCandidates_.empty()) {
        logPanel_->appendWarning("当前没有候选区域，请先点击“预览合并”。");
        setStatus("没有候选区域");
        return;
    }

    const auto filteredCandidates = filterCandidatesByType(lastMergeCandidates_, type);
    if (filteredCandidates.empty()) {
        viewer_->clearMergeCandidates();
        visibleMergeCandidateCount_ = 0;
        visibleMergeCandidateIds_.clear();
        refreshModelTree();
        const auto message = QString("当前没有 %1 类型候选。").arg(candidateTypeText(type));
        inspectPanel_->showReport(message);
        logPanel_->appendInfo(message);
        setStatus(message);
        return;
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
    showCandidateStatusReport(QString("已显示全部 %1 候选").arg(candidateTypeText(type)));
    setStatus(QString("已显示全部 %1 候选").arg(candidateTypeText(type)));
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
    const auto displayStatus = viewer_->displayDocument(document);
    if (!displayStatus.success()) {
        QMessageBox::critical(this, "显示模型失败", QString::fromStdString(displayStatus.message()));
        logPanel_->appendError(QString("合并后显示模型失败：%1").arg(QString::fromStdString(displayStatus.message())));
        setStatus("合并完成，但显示失败");
        refreshUndoRedoActions();
        return;
    }
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

    refreshDocumentViews(false);
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

    refreshDocumentViews(false);
    logPanel_->appendInfo("已重做上一条编辑命令。");
    setStatus("已重做");
    refreshUndoRedoActions();
}

void MainWindow::refreshUndoRedoActions() {
    const bool idle = !stlCropInProgress_ && !patchApplyInProgress_;
    undoAction_->setEnabled(idle && controller_.canUndo());
    redoAction_->setEnabled(idle && controller_.canRedo());
}

void MainWindow::refreshPatchApplyAction() {
    if (applyCurrentPatchAction_ == nullptr) {
        return;
    }

    const auto decision = controller_.currentPatchApplyDecision();
    const auto statusText = QString::fromUtf8(toString(controller_.currentPatchStatus()));
    const auto detail = decision.canRequestApply
        ? QString::fromStdString(decision.message)
        : QString::fromStdString(decision.reason);

    applyCurrentPatchAction_->setEnabled(decision.canRequestApply && !stlCropInProgress_ && !patchApplyInProgress_);
    applyCurrentPatchAction_->setToolTip(QString("Patch status：%1\n%2").arg(statusText, detail));
}

void MainWindow::refreshProcessStatusPanel() {
    if (processStatusPanel_ != nullptr) {
        processStatusPanel_->showStatus(controller_.currentProcessStatus());
    }
}

void MainWindow::startPatchPreviewProgressReport(const ProcessStatusSnapshot& initialStatus, const QString& header) {
    patchPreviewProgressActive_ = true;
    patchPreviewProgressHeader_ = header;
    patchPreviewProgressLines_.clear();
    patchPreviewLastProgress_ = initialStatus;
    patchPreviewProgressClock_.restart();

    if (patchPreviewProgressTimer_ == nullptr) {
        patchPreviewProgressTimer_ = new QTimer(this);
        patchPreviewProgressTimer_->setInterval(2000);
        connect(patchPreviewProgressTimer_, &QTimer::timeout, this, &MainWindow::refreshPatchPreviewProgressReport);
    }
    patchPreviewProgressTimer_->start();

    appendPatchPreviewProgress(initialStatus);
    if (bottomTabs_ != nullptr &&
        processStatusPanel_ != nullptr &&
        patchPreviewProgressHeader_.startsWith("Patch Apply")) {
        bottomTabs_->setCurrentWidget(processStatusPanel_);
    } else if (bottomTabs_ != nullptr && inspectPanel_ != nullptr) {
        bottomTabs_->setCurrentWidget(inspectPanel_->reportWidget());
    }
}

void MainWindow::appendPatchPreviewProgress(ProcessStatusSnapshot status, bool syncController) {
    if (!patchPreviewProgressActive_) {
        return;
    }

    patchPreviewLastProgress_ = std::move(status);
    if (syncController) {
        controller_.updateProcessStatus(patchPreviewLastProgress_);
        refreshProcessStatusPanel();
    } else if (processStatusPanel_ != nullptr) {
        processStatusPanel_->showStatus(patchPreviewLastProgress_);
    }

    patchPreviewProgressLines_ << patchPreviewProgressLine(patchPreviewLastProgress_);
    constexpr int maxProgressLineCount = 80;
    while (patchPreviewProgressLines_.size() > maxProgressLineCount) {
        patchPreviewProgressLines_.removeFirst();
    }

    refreshPatchPreviewProgressReport();
    setStatus(patchProgressStatusText());
}

void MainWindow::refreshPatchPreviewProgressReport() {
    if (!patchPreviewProgressActive_ || inspectPanel_ == nullptr) {
        return;
    }
    inspectPanel_->showReport(patchPreviewProgressReportText());
    setStatus(patchProgressStatusText());
}

void MainWindow::stopPatchPreviewProgressReport() {
    patchPreviewProgressActive_ = false;
    if (patchPreviewProgressTimer_ != nullptr) {
        patchPreviewProgressTimer_->stop();
    }
}

QString MainWindow::patchPreviewProgressReportText() const {
    QStringList lines;
    if (!patchPreviewProgressHeader_.isEmpty()) {
        lines << patchPreviewProgressHeader_;
    }

    const auto elapsed = patchPreviewProgressClock_.isValid() ? patchPreviewProgressClock_.elapsed() : 0;
    lines << QString("心跳：running  |  elapsed %1  |  last stage %2")
        .arg(elapsedText(elapsed))
        .arg(QString::fromLatin1(toString(patchPreviewLastProgress_.stage)));
    if (!patchPreviewLastProgress_.latestMessage.empty()) {
        lines << QString("最近消息：%1").arg(QString::fromStdString(patchPreviewLastProgress_.latestMessage));
    }
    if (!patchPreviewLastProgress_.latestWarning.empty()) {
        lines << QString("最近警告：%1").arg(QString::fromStdString(patchPreviewLastProgress_.latestWarning));
    }

    lines << "";
    lines << "阶段事件";
    lines << patchPreviewProgressLines_;
    return lines.join('\n');
}

QString MainWindow::patchPreviewProgressLine(const ProcessStatusSnapshot& status) const {
    const auto elapsed = patchPreviewProgressClock_.isValid() ? patchPreviewProgressClock_.elapsed() : 0;
    QStringList fields;
    fields << QString("[%1]").arg(elapsedText(elapsed));
    fields << QString::fromLatin1(toString(status.stage));
    if (status.candidateId >= 0) {
        fields << QString("candidate=%1").arg(status.candidateId);
    }
    if (status.sourceFaceCount > 0) {
        fields << QString("faces=%1").arg(status.sourceFaceCount);
    }
    if (status.boundaryEdgeCount > 0) {
        fields << QString("boundary_edges=%1").arg(status.boundaryEdgeCount);
    }
    if (!status.localStlPath.empty()) {
        fields << QString("local STL=%1").arg(pathToQString(status.localStlPath));
    }
    if (!status.patchStepPath.empty()) {
        fields << QString("patch STEP=%1").arg(pathToQString(status.patchStepPath));
    }
    if (!status.fitRegionLogPath.empty()) {
        fields << QString("fit_region log=%1").arg(pathToQString(status.fitRegionLogPath));
    }
    if (!status.patchPreviewRunLogPath.empty()) {
        fields << QString("run log=%1").arg(pathToQString(status.patchPreviewRunLogPath));
    }
    if (!status.latestMessage.empty()) {
        fields << QString("message=%1").arg(QString::fromStdString(status.latestMessage));
    }
    if (!status.latestWarning.empty()) {
        fields << QString("warning=%1").arg(QString::fromStdString(status.latestWarning));
    }
    return fields.join("  |  ");
}

QString MainWindow::patchProgressStatusText() const {
    QString statusPrefix = "Patch 预览";
    if (patchPreviewProgressHeader_.startsWith("Patch Apply")) {
        statusPrefix = "Patch Apply";
    } else if (patchPreviewProgressHeader_.startsWith("STEP/STP") ||
               patchPreviewProgressHeader_.startsWith("STEP ")) {
        statusPrefix = "STEP/STP";
    } else if (patchPreviewProgressHeader_.startsWith("合并候选")) {
        statusPrefix = "合并候选";
    }

    QStringList fields;
    fields << statusPrefix;
    fields << QString::fromLatin1(toString(patchPreviewLastProgress_.stage));
    const auto elapsed = patchPreviewProgressClock_.isValid() ? patchPreviewProgressClock_.elapsed() : 0;
    fields << QString("elapsed %1").arg(elapsedText(elapsed));
    if (patchPreviewLastProgress_.candidateId >= 0) {
        fields << QString("candidate %1").arg(patchPreviewLastProgress_.candidateId);
    }
    if (!patchPreviewLastProgress_.latestMessage.empty()) {
        fields << QString::fromStdString(patchPreviewLastProgress_.latestMessage);
    } else if (!patchPreviewLastProgress_.latestWarning.empty()) {
        fields << QString("warning: %1").arg(QString::fromStdString(patchPreviewLastProgress_.latestWarning));
    }
    return fields.join(" | ");
}

void MainWindow::publishCropBoundaryDiagnosticsStatus(const CropBoundaryDiagnosticsReport& diagnostics) {
    auto status = controller_.currentProcessStatus();
    status.cropBoundaryBandEvaluated = diagnostics.boundaryBandEvaluated;
    status.cropSourceTriangleAuditEvaluated = diagnostics.sourceTriangleAuditEvaluated;
    status.cropBoundaryBandSampleCount = diagnostics.boundaryBandSampleCount;
    status.cropBoundaryBandMissingPointCount = diagnostics.boundaryBandMissingPointCount;
    status.cropBoundaryBandMaxDistance = diagnostics.boundaryBandMaxDistance;
    status.cropRejectedNearBoundaryTriangleCount = diagnostics.rejectedNearBoundaryTriangleCount;
    status.cropConservativeKeepCandidateCount = diagnostics.conservativeKeepCandidateCount;
    if (!diagnostics.message.empty()) {
        status.latestMessage = diagnostics.message;
    }
    if (!diagnostics.warningMessage.empty()) {
        status.latestWarning = diagnostics.warningMessage;
    }
    controller_.updateProcessStatus(std::move(status));
}

void MainWindow::refreshDocumentViews(bool clearPatchState) {
    if (!controller_.hasDocument()) {
        return;
    }

    viewer_->clearPatchOverlay();
    if (clearPatchState) {
        controller_.clearCurrentPatchOverlay();
    }
    refreshPatchApplyAction();
    refreshProcessStatusPanel();

    const auto& document = controller_.document();
    clearMergeCandidateState();
    hasFeatureEdgeResult_ = false;
    refreshModelTree();
    const auto displayStatus = viewer_->displayDocument(document);
    if (!displayStatus.success()) {
        logPanel_->appendError(QString("刷新模型显示失败：%1").arg(QString::fromStdString(displayStatus.message())));
        setStatus("模型刷新失败");
        return;
    }
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
    const auto typeCounts = countCandidateTypes(lastMergeCandidates_);
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
        currentMergeCandidate(),
        &typeCounts,
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
    refreshPatchApplyAction();
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
        report += QString("\nFit Error：%1").arg(QString::number(info.fit_error, 'g', 6));
        if (info.matching_candidate_count > 1) {
            report += "\nNote：该 face 可能属于多个候选，当前显示第一个匹配项。";
        }
    } else {
        report += QString("\nAdjacent Protected Edges：%1\nAdjacent Locked Edges：%2")
            .arg(info.adjacent_protected_edge_count)
            .arg(info.adjacent_locked_edge_count);
        if (!hasCandidatePreview) {
            report += "\nNote：当前尚未生成合并候选，请先点击“预览合并”。";
        } else {
            report += "\nNote：当前候选检测只生成 FeatureBoundedRefit。若该面仍未进入候选，通常是因为区域面数不足，或邻接 protected/locked edge 阻断。";
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
    const auto* candidate = currentMergeCandidate();
    QString currentStatus = "无";
    if (candidate != nullptr) {
        currentStatus = candidateStatusText(candidate->status);
    }

    QString report = QString("%1\n当前候选 ID：%2\n当前候选状态：%3\nPending：%4\nAccepted：%5\nRejected：%6\nHidden：%7\n当前显示候选数量：%8")
        .arg(title)
        .arg(currentMergeCandidateId_ >= 0 ? QString::number(currentMergeCandidateId_) : "无")
        .arg(currentStatus)
        .arg(counts.pending)
        .arg(counts.accepted)
        .arg(counts.rejected)
        .arg(counts.hidden)
        .arg(visibleMergeCandidateCount_);
    if (candidate != nullptr) {
        report += QString("\n当前候选类型：%1\nface count：%2\nboundary edge count：%3\nrisk level：%4")
            .arg(candidateTypeText(candidate->candidate_type))
            .arg(candidate->face_count)
            .arg(candidate->boundary_edge_count)
            .arg(riskLevelText(candidate->risk_level));
    }

    inspectPanel_->showReport(report);
    QString logMessage = QString("%1：Pending %2，Accepted %3，Rejected %4，Hidden %5，当前显示 %6")
        .arg(title)
        .arg(counts.pending)
        .arg(counts.accepted)
        .arg(counts.rejected)
        .arg(counts.hidden)
        .arg(visibleMergeCandidateCount_);
    if (candidate != nullptr) {
        logMessage += QString("，当前候选 ID %1，类型 %2，face %3，boundary edge %4，risk %5，status %6")
            .arg(candidate->candidate_id)
            .arg(candidateTypeText(candidate->candidate_type))
            .arg(candidate->face_count)
            .arg(candidate->boundary_edge_count)
            .arg(riskLevelText(candidate->risk_level))
            .arg(candidateStatusText(candidate->status));
    }
    logPanel_->appendInfo(logMessage);
}

void MainWindow::setStlCropInProgress(bool inProgress) {
    stlCropInProgress_ = inProgress;
    openStepAction_->setEnabled(!inProgress);
    exportStepAction_->setEnabled(!inProgress);
    openSourceStlAction_->setEnabled(!inProgress);
    cropCurrentCandidateStlAction_->setEnabled(!inProgress);
    useConservativeStlCropAction_->setEnabled(!inProgress);
    useGlobalCutChainCropAction_->setEnabled(!inProgress);
    generateAndPreviewCurrentPatchAction_->setEnabled(!inProgress);
    useGeomagicRemeshAction_->setEnabled(!inProgress);
    importPatchForCurrentCandidateAction_->setEnabled(!inProgress);
    importPatchFromFileAction_->setEnabled(!inProgress);
    detectAction_->setEnabled(!inProgress);
    applyMergeAction_->setEnabled(!inProgress);
    validateAction_->setEnabled(!inProgress);
    acceptMergeCandidateAction_->setEnabled(!inProgress);
    rejectMergeCandidateAction_->setEnabled(!inProgress);
    hideMergeCandidateAction_->setEnabled(!inProgress);
    restoreMergeCandidateAction_->setEnabled(!inProgress);
    if (inProgress) {
        applyCurrentPatchAction_->setEnabled(false);
    } else {
        refreshPatchApplyAction();
    }
    previewMergeAction_->setEnabled(!inProgress);
    highlightMergeCandidateByIdAction_->setEnabled(!inProgress);
    refreshUndoRedoActions();
}

StlRegionExtractorOptions MainWindow::currentStlCropOptions() const {
    StlRegionExtractorOptions options;
    if (useGlobalCutChainCropAction_ != nullptr && useGlobalCutChainCropAction_->isChecked()) {
        options.mode = StlCropMode::GlobalCutChain;
    } else if (useConservativeStlCropAction_ != nullptr && useConservativeStlCropAction_->isChecked()) {
        options.mode = StlCropMode::ConservativeBoundaryBand;
    }
    return options;
}

void MainWindow::setGeomagicFittingInputMode(GeomagicFittingInputMode mode) {
    fittingInputMode_ = mode;
    updateFittingInputModeActions();
}

GeomagicFittingInputMode MainWindow::currentFittingInputMode() const {
    if (fittingModeStpSampledAction_ != nullptr && fittingModeStpSampledAction_->isChecked()) {
        return GeomagicFittingInputMode::StpSampledCandidateSurface;
    }
    if (fittingModeConservativeBandAction_ != nullptr && fittingModeConservativeBandAction_->isChecked()) {
        return GeomagicFittingInputMode::ConservativeBoundaryBandStlCrop;
    }
    return GeomagicFittingInputMode::LegacyStlCrop;
}

void MainWindow::updateFittingInputModeActions() {
    fittingModeLegacyStlCropAction_->setChecked(
        fittingInputMode_ == GeomagicFittingInputMode::LegacyStlCrop);
    fittingModeConservativeBandAction_->setChecked(
        fittingInputMode_ == GeomagicFittingInputMode::ConservativeBoundaryBandStlCrop);
    fittingModeStpSampledAction_->setChecked(
        fittingInputMode_ == GeomagicFittingInputMode::StpSampledCandidateSurface);
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
