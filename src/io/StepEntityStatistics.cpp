#include "io/StepEntityStatistics.h"

#include <array>
#include <cctype>

namespace spo {

namespace {

bool containsEntityName(
    const std::string_view body,
    const std::string_view name) {
    auto position = body.find(name);
    while (position != std::string_view::npos) {
        const auto nameEnd = position + name.size();
        const auto leftBoundary = position == 0 ||
            std::isspace(static_cast<unsigned char>(body[position - 1])) ||
            body[position - 1] == '(' ||
            body[position - 1] == ',';
        auto next = nameEnd;
        while (next < body.size() &&
               std::isspace(static_cast<unsigned char>(body[next]))) {
            ++next;
        }
        if (leftBoundary && next < body.size() && body[next] == '(') {
            return true;
        }
        position = body.find(name, position + 1);
    }
    return false;
}

}

StepEntityLineClassification classifyStepEntityLine(
    std::string_view line) {
    StepEntityLineClassification result;
    while (!line.empty() &&
           std::isspace(static_cast<unsigned char>(line.front()))) {
        line.remove_prefix(1);
    }
    const auto equals = line.find('=');
    if (line.empty() || line.front() != '#' ||
        equals == std::string_view::npos) {
        return result;
    }
    result.entity = true;
    const auto body = line.substr(equals + 1);
    result.edgeCurve = containsEntityName(body, "EDGE_CURVE");
    result.advancedFace = containsEntityName(body, "ADVANCED_FACE");
    result.bSplineSurface =
        containsEntityName(body, "B_SPLINE_SURFACE_WITH_KNOTS") ||
        containsEntityName(body, "B_SPLINE_SURFACE");
    static constexpr std::array<std::string_view, 10> surfaceNames {
        "PLANE",
        "CYLINDRICAL_SURFACE",
        "CONICAL_SURFACE",
        "SPHERICAL_SURFACE",
        "TOROIDAL_SURFACE",
        "B_SPLINE_SURFACE_WITH_KNOTS",
        "BEZIER_SURFACE",
        "SURFACE_OF_REVOLUTION",
        "SURFACE_OF_LINEAR_EXTRUSION",
        "OFFSET_SURFACE"};
    for (const auto name : surfaceNames) {
        if (containsEntityName(body, name)) {
            result.surface = true;
            break;
        }
    }
    return result;
}

void accumulateStepEntityLine(
    StepEntityStatistics& statistics,
    const std::string_view line) {
    const auto classification = classifyStepEntityLine(line);
    statistics.entityCount += classification.entity ? 1 : 0;
    statistics.edgeCurveCount += classification.edgeCurve ? 1 : 0;
    statistics.surfaceCount += classification.surface ? 1 : 0;
    statistics.advancedFaceCount +=
        classification.advancedFace ? 1 : 0;
    statistics.bSplineSurfaceCount +=
        classification.bSplineSurface ? 1 : 0;
}

}
