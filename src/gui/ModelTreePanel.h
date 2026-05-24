#pragma once

#include "brep/ShapeDocument.h"
#include "feature/FeatureEdgeDetector.h"

#include <QWidget>

class QTreeWidget;

namespace spo {

class ModelTreePanel final : public QWidget {
public:
    explicit ModelTreePanel(QWidget* parent = nullptr);
    void showEmpty();
    void showDocument(
        const ShapeDocument& document,
        int lockedEdgeCount = 0,
        int mergeCandidateCount = 0,
        int visibleMergeCandidateCount = 0,
        int pendingCandidateCount = 0,
        int acceptedCandidateCount = 0,
        int rejectedCandidateCount = 0,
        int hiddenCandidateCount = 0,
        int currentMergeCandidateId = -1,
        const FeatureEdgeDetectionResult* featureEdges = nullptr);

private:
    QTreeWidget* tree_ = nullptr;
};

}
