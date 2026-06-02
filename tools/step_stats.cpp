#include "common/Config.h"
#include "patch/PatchImportService.h"

#include <iostream>

int main(int argc, char* argv[]) {
    std::cout << spo::kApplicationName << " STEP stats\n";
    if (argc != 2) {
        std::cerr << "Usage: step_stats <patch.stp|patch.step|patch.igs|patch.iges>\n";
        return 2;
    }

    const auto result = spo::PatchImportService().importPatch(argv[1]);
    std::cout << "path: " << argv[1] << "\n";
    std::cout << "success: " << result.success << "\n";
    std::cout << "faces: " << result.faceCount << "\n";
    std::cout << "edges: " << result.edgeCount << "\n";
    std::cout << "shells: " << result.shellCount << "\n";
    std::cout << "solids: " << result.solidCount << "\n";
    std::cout << "bbox_valid: " << result.bboxValid << "\n";
    std::cout << "brep_check_valid: " << result.brepCheckValid << "\n";
    if (!result.message.empty()) {
        std::cout << "message: " << result.message << "\n";
    }
    if (!result.errorMessage.empty()) {
        std::cerr << "error: " << result.errorMessage << "\n";
    }
    return result.success ? 0 : 1;
}
