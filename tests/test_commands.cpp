#include "command/CommandContext.h"
#include "command/DetectFeatureCommand.h"
#include "command/ExportStepCommand.h"
#include "command/LoadStepCommand.h"
#include "command/MergePatchCommand.h"
#include "command/ValidateShapeCommand.h"
#include "brep/ShapeDocument.h"
#include "io/StepWriter.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <cassert>
#include <chrono>
#include <filesystem>

namespace {

std::filesystem::path temp_step_path(const char* suffix) {
    return std::filesystem::temp_directory_path() /
        ("step-patch-optimizer-command-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + suffix);
}

std::filesystem::path create_test_step() {
    const auto path = temp_step_path(".stp");
    const auto shape = BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape();
    const spo::ShapeDocument document(shape, path);
    const spo::StepWriter writer;
    assert(writer.write(document, path).success());
    return path;
}

}

void run_command_tests() {
    const auto sample = create_test_step();

    spo::CommandContext context;

    spo::LoadStepCommand load(sample);
    assert(load.execute(context).success());
    assert(context.document.hasShape());

    spo::DetectFeatureCommand detect(25.0, 0.0);
    assert(detect.execute(context).success());

    spo::MergePatchCommand merge(25.0, 0.0, 0.001);
    assert(merge.execute(context).success());
    assert(context.document.hasShape());

    spo::ValidateShapeCommand validate;
    assert(validate.execute(context).success());
    assert(context.validationReport.has_shape);

    const auto output = temp_step_path("-export.stp");
    spo::ExportStepCommand exportStep(output);
    assert(exportStep.execute(context).success());
    std::filesystem::remove(output);
    std::filesystem::remove(sample);
}
