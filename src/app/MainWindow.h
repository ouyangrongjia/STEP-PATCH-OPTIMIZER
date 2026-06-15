#pragma once

#include "app/AppController.h"
#include "merge/FaceInspectInfo.h"
#include "stl/StlRegionExtractor.h"

#include <QMainWindow>
#include <QElapsedTimer>
#include <QStringList>

#include <set>
#include <vector>

class QAction;
class QDockWidget;
class QMenu;
class QTabWidget;
class QTimer;

namespace spo {

class InspectPanel;
class LogPanel;
class ModelTreePanel;
class OccViewWidget;
class ParameterPanel;
class ProcessStatusPanel;

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void createActions();
    void createMenus();
    void createToolBars();
    void createDocks();
    void connectActions();

    void openStepFile();
    void saveProject();
    void exportStepFile();
    void openSourceStlFile();
    void cropCurrentCandidateStl();
    void generateAndPreviewCurrentPatch();
    void importPatchForCurrentCandidate();
    void importPatchFromFile();
    void applyCurrentPatchPreview();
    void clearPatchOverlay();
    void detectFeatureEdges();
    void previewMergeCandidates();
    void showAllMergeCandidates();
    void highlightMergeCandidateById();
    void selectMergeCandidateByFace(FaceId faceId);
    void clearMergeCandidatePreview();
    void acceptCurrentMergeCandidate();
    void rejectCurrentMergeCandidate();
    void hideCurrentMergeCandidate();
    void restoreCurrentMergeCandidate();
    void showAcceptedMergeCandidates();
    void showPendingMergeCandidates();
    void showNonHiddenMergeCandidates();
    void showStrictPlaneMergeCandidates();
    void showMergeCandidatesByTypeDialog();
    void showMergeCandidatesByType(MergeCandidateType type);
    void mergeCurrentPlaneCandidate();
    void mergeAcceptedPlaneCandidates();
    void mergeAllMergeablePlaneCandidates();
    void mergeCurrentApproximatePlaneCandidate();
    void mergeAllApproximatePlaneCandidates();
    void mergeApproximatePlaneCandidateBatch(const std::vector<MergeCandidate>& candidates, const QString& title);
    void mergePlaneCandidateBatch(const std::vector<MergeCandidate>& candidates, const QString& title, bool approximateMode);
    void mergeCurrentSphereCandidate();
    void mergeAcceptedSphereCandidates();
    void mergeAllMergeableSphereCandidates();
    void mergeSphereCandidateBatch(const std::vector<MergeCandidate>& candidates, const QString& title);
    void applyMerge();
    void validateShape();
    void resetView();
    void undo();
    void redo();
    void refreshUndoRedoActions();
    void refreshDocumentViews(bool clearPatchState = true);
    void syncLockedEdges();
    void refreshModelTree();
    void clearMergeCandidateState();
    void showFaceInspectReport(const FaceInspectInfo& info, bool hasCandidatePreview);
    MergeCandidate* currentMergeCandidate();
    bool setCurrentMergeCandidateStatus(MergeCandidateStatus status);
    void showFilteredMergeCandidates(MergeCandidateStatus status);
    void showCandidateStatusReport(const QString& title);
    void showPatchPreviewReport(
        const PatchPreviewReport& report,
        bool visualCutoutPreview = false,
        const CropBoundaryDiagnosticsReport* diagnostics = nullptr,
        const QString& cropMode = {});
    void refreshPatchApplyAction();
    void refreshProcessStatusPanel();
    void publishCropBoundaryDiagnosticsStatus(const CropBoundaryDiagnosticsReport& diagnostics);
    void startPatchPreviewProgressReport(const ProcessStatusSnapshot& initialStatus, const QString& header);
    void appendPatchPreviewProgress(ProcessStatusSnapshot status, bool syncController = true);
    void refreshPatchPreviewProgressReport();
    void stopPatchPreviewProgressReport();
    QString patchPreviewProgressReportText() const;
    QString patchPreviewProgressLine(const ProcessStatusSnapshot& status) const;
    void setStlCropInProgress(bool inProgress);
    StlRegionExtractorOptions currentStlCropOptions() const;
    void setGeomagicFittingInputMode(GeomagicFittingInputMode mode);
    GeomagicFittingInputMode currentFittingInputMode() const;
    void updateFittingInputModeActions();
    void lockSelectedEdges(const std::vector<EdgeId>& edgeIds);
    void unlockSelectedEdges(const std::vector<EdgeId>& edgeIds);
    void setStatus(const QString& message);

    AppController controller_;
    OccViewWidget* viewer_ = nullptr;
    ModelTreePanel* modelTree_ = nullptr;
    ParameterPanel* parameterPanel_ = nullptr;
    InspectPanel* inspectPanel_ = nullptr;
    LogPanel* logPanel_ = nullptr;
    ProcessStatusPanel* processStatusPanel_ = nullptr;
    QTabWidget* bottomTabs_ = nullptr;
    QDockWidget* modelDock_ = nullptr;
    QDockWidget* parameterDock_ = nullptr;
    QDockWidget* bottomDock_ = nullptr;
    QMenu* viewMenu_ = nullptr;
    QMenu* stlMenu_ = nullptr;
    QMenu* patchMenu_ = nullptr;
    QMenu* planeMergeMenu_ = nullptr;
    QMenu* sphereMergeMenu_ = nullptr;

    QAction* openStepAction_ = nullptr;
    QAction* saveProjectAction_ = nullptr;
    QAction* exportStepAction_ = nullptr;
    QAction* openSourceStlAction_ = nullptr;
    QAction* cropCurrentCandidateStlAction_ = nullptr;
    QAction* useConservativeStlCropAction_ = nullptr;
    QAction* useGlobalCutChainCropAction_ = nullptr;
    QAction* showSourceStlAction_ = nullptr;
    QAction* showCroppedStlAction_ = nullptr;
    QAction* showStlCropBoxAction_ = nullptr;
    QAction* generateAndPreviewCurrentPatchAction_ = nullptr;
    QAction* useGeomagicRemeshAction_ = nullptr;
    QAction* fittingModeLegacyStlCropAction_ = nullptr;
    QAction* fittingModeConservativeBandAction_ = nullptr;
    QAction* fittingModeStpSampledAction_ = nullptr;
    QAction* importPatchForCurrentCandidateAction_ = nullptr;
    QAction* importPatchFromFileAction_ = nullptr;
    QAction* applyCurrentPatchAction_ = nullptr;
    QAction* clearPatchOverlayAction_ = nullptr;
    QAction* exitAction_ = nullptr;
    QAction* selectFaceAction_ = nullptr;
    QAction* selectEdgeAction_ = nullptr;
    QAction* selectCandidateAction_ = nullptr;
    QAction* detectAction_ = nullptr;
    QAction* previewMergeAction_ = nullptr;
    QAction* showAllMergeCandidatesAction_ = nullptr;
    QAction* highlightMergeCandidateByIdAction_ = nullptr;
    QAction* clearMergeCandidatesAction_ = nullptr;
    QAction* acceptMergeCandidateAction_ = nullptr;
    QAction* rejectMergeCandidateAction_ = nullptr;
    QAction* hideMergeCandidateAction_ = nullptr;
    QAction* restoreMergeCandidateAction_ = nullptr;
    QAction* showAcceptedMergeCandidatesAction_ = nullptr;
    QAction* showPendingMergeCandidatesAction_ = nullptr;
    QAction* showCandidatesByTypeAction_ = nullptr;
    QAction* mergePlaneCandidateAction_ = nullptr;
    QAction* mergeAcceptedPlaneCandidatesAction_ = nullptr;
    QAction* mergeAllPlaneCandidatesAction_ = nullptr;
    QAction* mergeApproximatePlaneCandidateAction_ = nullptr;
    QAction* mergeAllApproximatePlaneCandidatesAction_ = nullptr;
    QAction* mergeSphereCandidateAction_ = nullptr;
    QAction* mergeAcceptedSphereCandidatesAction_ = nullptr;
    QAction* mergeAllSphereCandidatesAction_ = nullptr;
    QAction* applyMergeAction_ = nullptr;
    QAction* validateAction_ = nullptr;
    QAction* resetViewAction_ = nullptr;
    QAction* fitAllAction_ = nullptr;
    QAction* toggleFeaturesAction_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    std::vector<MergeCandidate> lastMergeCandidates_;
    std::set<int> visibleMergeCandidateIds_;
    int visibleMergeCandidateCount_ = 0;
    int currentMergeCandidateId_ = -1;
    bool hasFeatureEdgeResult_ = false;
    bool stlCropInProgress_ = false;
    bool patchApplyInProgress_ = false;
    QTimer* patchPreviewProgressTimer_ = nullptr;
    QString patchPreviewProgressHeader_;
    QStringList patchPreviewProgressLines_;
    ProcessStatusSnapshot patchPreviewLastProgress_;
    QElapsedTimer patchPreviewProgressClock_;
    bool patchPreviewProgressActive_ = false;
    GeomagicFittingInputMode fittingInputMode_ = GeomagicFittingInputMode::StpSampledCandidateSurface;
};

}
