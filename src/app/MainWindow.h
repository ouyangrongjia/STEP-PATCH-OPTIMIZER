#pragma once

#include "app/AppController.h"

#include <QMainWindow>

#include <vector>

class QAction;
class QDockWidget;
class QMenu;
class QTabWidget;

namespace spo {

class InspectPanel;
class LogPanel;
class ModelTreePanel;
class OccViewWidget;
class ParameterPanel;

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
    void detectFeatureEdges();
    void previewMergeCandidates();
    void applyMerge();
    void validateShape();
    void resetView();
    void undo();
    void redo();
    void refreshUndoRedoActions();
    void refreshDocumentViews();
    void syncLockedEdges();
    void lockSelectedEdges(const std::vector<EdgeId>& edgeIds);
    void unlockSelectedEdges(const std::vector<EdgeId>& edgeIds);
    void setStatus(const QString& message);

    AppController controller_;
    OccViewWidget* viewer_ = nullptr;
    ModelTreePanel* modelTree_ = nullptr;
    ParameterPanel* parameterPanel_ = nullptr;
    InspectPanel* inspectPanel_ = nullptr;
    LogPanel* logPanel_ = nullptr;
    QTabWidget* bottomTabs_ = nullptr;
    QDockWidget* modelDock_ = nullptr;
    QDockWidget* parameterDock_ = nullptr;
    QDockWidget* bottomDock_ = nullptr;
    QMenu* viewMenu_ = nullptr;

    QAction* openStepAction_ = nullptr;
    QAction* saveProjectAction_ = nullptr;
    QAction* exportStepAction_ = nullptr;
    QAction* exitAction_ = nullptr;
    QAction* selectFaceAction_ = nullptr;
    QAction* selectEdgeAction_ = nullptr;
    QAction* selectCandidateAction_ = nullptr;
    QAction* detectAction_ = nullptr;
    QAction* previewMergeAction_ = nullptr;
    QAction* applyMergeAction_ = nullptr;
    QAction* validateAction_ = nullptr;
    QAction* resetViewAction_ = nullptr;
    QAction* fitAllAction_ = nullptr;
    QAction* toggleFeaturesAction_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
};

}
