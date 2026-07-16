#pragma once

#include "app/ProcessStatus.h"
#include "external/geomagic/GeomagicAutoSurfaceConfig.h"
#include "merge/MergeCandidate.h"

#include <QElapsedTimer>
#include <QString>
#include <QStringList>

#include <set>
#include <vector>

class QTimer;

namespace spo {

struct MainWindowWorkflowState {
    std::vector<MergeCandidate> candidates;
    std::set<int> visibleCandidateIds;
    int visibleCandidateCount = 0;
    int currentCandidateId = -1;
    bool hasFeatureEdgeResult = false;

    bool stlCropInProgress = false;
    bool patchApplyInProgress = false;
    QTimer* progressTimer = nullptr;
    QString progressHeader;
    QStringList progressLines;
    ProcessStatusSnapshot lastProgress;
    QElapsedTimer progressClock;
    bool progressActive = false;
    GeomagicFittingInputMode fittingInputMode =
        GeomagicFittingInputMode::StpSampledCandidateSurface;

    void clearCandidates() {
        candidates.clear();
        visibleCandidateIds.clear();
        visibleCandidateCount = 0;
        currentCandidateId = -1;
    }

    qint64 elapsedMilliseconds() const {
        return progressClock.isValid() ? progressClock.elapsed() : 0;
    }
};

}
