#include "io/StepReader.h"
#include "io/StepEntityStatistics.h"

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
    assert(statistics.entityCount == 2);
    assert(statistics.surfaceCount == 1);
    assert(statistics.edgeCurveCount == 1);
    test_step_reader_accepts_step_extensions();
}
