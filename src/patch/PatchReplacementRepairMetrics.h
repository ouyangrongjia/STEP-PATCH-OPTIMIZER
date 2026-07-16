#pragma once

namespace spo {

struct PatchReplacementRepairMetrics {
    bool sameParameterApplied = false;
    bool shapeFixShapeApplied = false;
    bool shapeFixFaceApplied = false;
    bool shapeFixWireApplied = false;
    bool unifySameDomainApplied = false;
    bool sewingApplied = false;
    bool adaptiveSewingApplied = false;
    bool shellToSolidApplied = false;

    int faceCountBeforeRepair = 0;
    int edgeCountBeforeRepair = 0;
    int shellCountBeforeRepair = 0;
    int solidCountBeforeRepair = 0;
    int faceCountAfterRepair = 0;
    int edgeCountAfterRepair = 0;
    int shellCountAfterRepair = 0;
    int solidCountAfterRepair = 0;

    int freeEdgesBeforeRepair = 0;
    int freeEdgesAfterRepair = 0;
    int multipleEdgesBeforeRepair = 0;
    int multipleEdgesAfterRepair = 0;
    int degeneratedFreeEdgesBeforeRepair = 0;
    int degeneratedFreeEdgesAfterRepair = 0;

    double selectedSewingTolerance = 0.0;
    int sewingAttemptCount = 0;
    int bestSewingFreeEdges = 0;
    int bestSewingMultipleEdges = 0;
    int bestSewingDegeneratedFreeEdgeCount = 0;
    int bestSewingFaceCount = 0;
    int bestSewingEdgeCount = 0;
    int bestSewingShellCount = 0;
    int bestSewingSolidCount = 0;
    bool bestSewingBRepCheckValid = false;
    bool bestSewingCollapsed = false;
};

}
