#pragma once

#include "brep/ShapeDocument.h"

#include <filesystem>
#include <string>

namespace spo {

struct PatchReplacementReport;

enum class StrictTopologyFailureReason {
    None,
    MissingBeforeShape,
    MissingAfterShape,
    BRepCheckFailed,
    FreeEdgeIncreased,
    MultipleEdgeIncreased,
    SolidCountChangedUnexpectedly,
    ShellCountInvalid,
    BBoxInvalid,
    BBoxDeviationTooLarge,
    StepExportFailed,
    StepRoundtripFailed,
    ReplacementStatsInvalid
};

const char* toString(StrictTopologyFailureReason reason);

struct StrictTopologyGateInput {
    const ShapeDocument* beforeDocument = nullptr;
    const ShapeDocument* afterDocument = nullptr;
    const PatchReplacementReport* replacementReport = nullptr;

    bool requireStepRoundtrip = true;
    bool allowMultiFaceReplacement = true;
    bool allowFaceCountIncrease = true;

    std::filesystem::path temporaryStepPath;
};

struct StrictTopologyGateReport {
    bool passed = false;
    StrictTopologyFailureReason failureReason = StrictTopologyFailureReason::None;

    ShapeStats beforeStats;
    ShapeStats afterStats;

    int beforeFreeEdges = 0;
    int afterFreeEdges = 0;
    int beforeMultipleEdges = 0;
    int afterMultipleEdges = 0;

    bool beforeBRepCheckValid = false;
    bool afterBRepCheckValid = false;
    bool stepExportOk = false;
    bool stepRoundtripOk = false;

    bool multiFaceReplacement = false;
    int replacementFaceCount = 0;

    std::string message;
    std::string warningMessage;
};

class StrictTopologyGate {
public:
    StrictTopologyGateReport evaluate(const StrictTopologyGateInput& input) const;
};

}
