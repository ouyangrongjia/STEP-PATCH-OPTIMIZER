#include "io/StepReader.h"

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
    test_step_reader_accepts_step_extensions();
}
