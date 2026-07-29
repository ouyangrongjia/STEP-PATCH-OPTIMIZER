#include "io/StepReader.h"
#include "io/StepEntityStatistics.h"
#include "io/StepFaceLineage.h"

#include <cassert>

namespace {

void test_step_reader_accepts_step_extensions() {
    const spo::StepReader reader;
    assert(reader.canRead("model.step").success());
    assert(reader.canRead("model.stp").success());
    assert(!reader.canRead("model.txt").success());
}

}

void run_step_io_tests() {
    const auto spacedSurface = spo::classifyStepEntityLine(
        "#42 = B_SPLINE_SURFACE_WITH_KNOTS ( 'surface', 3, 3 );");
    assert(spacedSurface.entity);
    assert(spacedSurface.surface);
    assert(spacedSurface.bSplineSurface);
    const auto spacedEdge = spo::classifyStepEntityLine(
        "#43 = EDGE_CURVE ( 'edge', #1, #2, #3, .T. );");
    assert(spacedEdge.entity);
    assert(spacedEdge.edgeCurve);
    assert(!spacedEdge.surface);
    spo::StepEntityStatistics statistics;
    spo::accumulateStepEntityLine(
        statistics,
        "#42 = B_SPLINE_SURFACE_WITH_KNOTS ( 'surface', 3, 3 );");
    spo::accumulateStepEntityLine(
        statistics,
        "#43 = EDGE_CURVE ( 'edge', #1, #2, #3, .T. );");
    spo::accumulateStepEntityLine(
        statistics,
        "#44 = ADVANCED_FACE ( 'replacement', (#10), #42, .T. );");
    assert(statistics.entityCount == 3);
    assert(statistics.surfaceCount == 1);
    assert(statistics.edgeCurveCount == 1);
    assert(statistics.bSplineSurfaceCount == 1);
    assert(statistics.advancedFaceCount == 1);
    const auto lineage = spo::makeStepFaceLineageReport(
        2739,
        49,
        4,
        spo::StepEntityStatistics {0, 0, 4, 4, 4},
        spo::StepEntityStatistics {0, 0, 2694, 2694, 4});
    assert(lineage.expectedWholeFaceCount == 2694);
    assert(lineage.replacementCountPassed);
    assert(lineage.wholeCountPassed);
    assert(lineage.passed);
    test_step_reader_accepts_step_extensions();
}
