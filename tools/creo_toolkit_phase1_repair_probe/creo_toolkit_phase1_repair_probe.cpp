#include <ProCore.h>
#include <ProFeature.h>
#include <ProImportfeat.h>
#include <ProIntf3Dexport.h>
#include <ProIntfData.h>
#include <ProIntfimport.h>
#include <ProMdl.h>
#include <ProMdlChk.h>
#include <ProSolid.h>
#include <ProToolkit.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

extern "C" ProError user_initialize(
    int /*argc*/,
    char** /*argv*/,
    char* /*version*/,
    char* /*build*/,
    wchar_t /*errbuf*/[80]) {
    return PRO_TK_NO_ERROR;
}

extern "C" void user_terminate() {
}

namespace {

struct ProbeOptions {
    std::string baseRemovedStep;
    std::string geomagicPatchStep;
    std::string creoCommand;
    std::string textPath;
    std::string outputDir;
    std::string modelcheckOutputDir;
    std::string exportStepBase;
    std::string resultJson;
    std::string modelName = "spo_phase1_base";
};

struct CallResult {
    std::string name;
    ProError code = PRO_TK_NO_ERROR;
};

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (const char c : value) {
        switch (c) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out << "\\u00";
                const char* digits = "0123456789ABCDEF";
                out << digits[(static_cast<unsigned char>(c) >> 4) & 0xF];
                out << digits[static_cast<unsigned char>(c) & 0xF];
            } else {
                out << c;
            }
            break;
        }
    }
    return out.str();
}

std::string quote_json(const std::string& value) {
    return "\"" + json_escape(value) + "\"";
}

std::string bool_json(const bool value) {
    return value ? "true" : "false";
}

std::string narrow_from_wide(const wchar_t* value) {
    if (value == nullptr) {
        return {};
    }
#ifdef _WIN32
    const int required = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(required - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), required, nullptr, nullptr);
    return result;
#else
    std::wstring wide(value);
    return std::string(wide.begin(), wide.end());
#endif
}

bool copy_to_pro_path(const std::string& value, ProPath path) {
    std::fill(path, path + PRO_PATH_SIZE, L'\0');
    if (value.empty()) {
        return true;
    }
#ifdef _WIN32
    const int required = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (required <= 0 || required > PRO_PATH_SIZE) {
        return false;
    }
    return MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, path, PRO_PATH_SIZE) > 0;
#else
    if (value.size() >= PRO_PATH_SIZE) {
        return false;
    }
    for (size_t i = 0; i < value.size(); ++i) {
        path[i] = static_cast<wchar_t>(value[i]);
    }
    return true;
#endif
}

bool copy_to_model_name(const std::string& value, ProMdlName name) {
    std::fill(name, name + PRO_NAME_SIZE, L'\0');
    if (value.empty() || value.size() >= PRO_NAME_SIZE) {
        return false;
    }
    for (size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        name[i] = static_cast<wchar_t>((std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_');
    }
    return true;
}

std::map<std::string, std::string> parse_args(const int argc, char** argv) {
    std::map<std::string, std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::string key = argv[i];
        if (key.rfind("--", 0) != 0) {
            continue;
        }
        if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
            args[key.substr(2)] = argv[++i];
        } else {
            args[key.substr(2)] = "1";
        }
    }
    return args;
}

ProbeOptions options_from_args(const int argc, char** argv) {
    const auto args = parse_args(argc, argv);
    ProbeOptions options;
    auto get = [&](const std::string& key) -> std::string {
        const auto it = args.find(key);
        return it == args.end() ? std::string{} : it->second;
    };

    options.baseRemovedStep = get("base-removed-step");
    options.geomagicPatchStep = get("geomagic-patch-step");
    options.creoCommand = get("creo-command");
    options.textPath = get("text-path");
    options.outputDir = get("output-dir");
    options.modelcheckOutputDir = get("modelcheck-output-dir");
    options.exportStepBase = get("export-step-base");
    options.resultJson = get("result");
    const auto modelName = get("model-name");
    if (!modelName.empty()) {
        options.modelName = modelName;
    }
    return options;
}

std::vector<std::string> validate_options(const ProbeOptions& options) {
    std::vector<std::string> errors;
    if (options.baseRemovedStep.empty()) {
        errors.push_back("--base-removed-step is required.");
    }
    if (options.geomagicPatchStep.empty()) {
        errors.push_back("--geomagic-patch-step is required.");
    }
    if (options.creoCommand.empty()) {
        errors.push_back("--creo-command is required.");
    }
    if (options.outputDir.empty()) {
        errors.push_back("--output-dir is required.");
    }
    if (options.modelcheckOutputDir.empty()) {
        errors.push_back("--modelcheck-output-dir is required.");
    }
    if (options.exportStepBase.empty()) {
        errors.push_back("--export-step-base is required.");
    }
    if (options.resultJson.empty()) {
        errors.push_back("--result is required.");
    }
    if (!options.baseRemovedStep.empty() && !std::filesystem::exists(options.baseRemovedStep)) {
        errors.push_back("--base-removed-step does not exist: " + options.baseRemovedStep);
    }
    if (!options.geomagicPatchStep.empty() && !std::filesystem::exists(options.geomagicPatchStep)) {
        errors.push_back("--geomagic-patch-step does not exist: " + options.geomagicPatchStep);
    }
    return errors;
}

bool contains_fail_signal(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value.find("FAIL") != std::string::npos ||
        value.find("SOLID_FAILED") != std::string::npos;
}

void write_result(
    const ProbeOptions& options,
    const std::string& status,
    const bool runnerSuccess,
    const bool stage1AcceptedByToolkit,
    const std::vector<CallResult>& calls,
    const std::string& validationScore,
    const ProError validationScoreCode,
    const int modelcheckErrors,
    const int modelcheckWarnings,
    const ProBoolean modelSavedByModelcheck,
    const std::string& exportedStepBase,
    const bool exportedStepMaybeExists,
    const std::vector<std::string>& notes) {
    std::filesystem::create_directories(std::filesystem::path(options.resultJson).parent_path());
    std::ofstream out(options.resultJson, std::ios::binary);
    out << "{\n";
    out << "  \"tool\": \"creo_toolkit_phase1_repair_probe\",\n";
    out << "  \"status\": " << quote_json(status) << ",\n";
    out << "  \"runner_success\": " << bool_json(runnerSuccess) << ",\n";
    out << "  \"stage1_toolkit_acceptance_passed\": " << bool_json(stage1AcceptedByToolkit) << ",\n";
    out << "  \"acceptance_rule\": \"Phase 1 requires ProImportfeatCreate with join_surfaces/attempt_make_solid, regenerate/save/modelcheck/export, then downstream OCCT StrictTopologyGate watertight validation. SHORT_EDGES alone is diagnostic-only.\",\n";
    out << "  \"short_edges_diagnostic_only\": true,\n";
    out << "  \"base_removed_candidate_step\": " << quote_json(options.baseRemovedStep) << ",\n";
    out << "  \"geomagic_patch_step\": " << quote_json(options.geomagicPatchStep) << ",\n";
    out << "  \"output_dir\": " << quote_json(options.outputDir) << ",\n";
    out << "  \"modelcheck_output_dir\": " << quote_json(options.modelcheckOutputDir) << ",\n";
    out << "  \"import_feature_attributes\": {\n";
    out << "    \"join_surfaces\": true,\n";
    out << "    \"attempt_make_solid\": true,\n";
    out << "    \"cut_or_add\": false,\n";
    out << "    \"add_bodies\": false\n";
    out << "  },\n";
    out << "  \"export_step_base\": " << quote_json(exportedStepBase) << ",\n";
    out << "  \"export_step_maybe_exists\": " << bool_json(exportedStepMaybeExists) << ",\n";
    out << "  \"import_validation_score_get_code\": " << validationScoreCode << ",\n";
    out << "  \"import_validation_score\": " << quote_json(validationScore) << ",\n";
    out << "  \"modelcheck_errors\": " << modelcheckErrors << ",\n";
    out << "  \"modelcheck_warnings\": " << modelcheckWarnings << ",\n";
    out << "  \"modelcheck_model_saved\": " << bool_json(modelSavedByModelcheck == PRO_B_TRUE) << ",\n";
    out << "  \"calls\": [\n";
    for (size_t i = 0; i < calls.size(); ++i) {
        out << "    {\"name\": " << quote_json(calls[i].name) << ", \"code\": " << calls[i].code << "}";
        out << (i + 1 == calls.size() ? "\n" : ",\n");
    }
    out << "  ],\n";
    out << "  \"notes\": [\n";
    for (size_t i = 0; i < notes.size(); ++i) {
        out << "    " << quote_json(notes[i]);
        out << (i + 1 == notes.size() ? "\n" : ",\n");
    }
    out << "  ]\n";
    out << "}\n";
}

void write_and_end(
    const ProbeOptions& options,
    const std::string& status,
    const int exitCode,
    const bool runnerSuccess,
    const bool stage1AcceptedByToolkit,
    std::vector<CallResult>& calls,
    const std::string& validationScore,
    const ProError validationScoreCode,
    const int modelcheckErrors,
    const int modelcheckWarnings,
    const ProBoolean modelSavedByModelcheck,
    const std::vector<std::string>& notes,
    const bool creoStarted) {
    if (creoStarted) {
        const auto endCode = ProEngineerEnd();
        calls.push_back({"ProEngineerEnd", endCode});
    }
    const bool exportedStepMaybeExists =
        std::filesystem::exists(options.exportStepBase + ".stp") ||
        std::filesystem::exists(options.exportStepBase + ".step");
    write_result(
        options,
        status,
        runnerSuccess,
        stage1AcceptedByToolkit,
        calls,
        validationScore,
        validationScoreCode,
        modelcheckErrors,
        modelcheckWarnings,
        modelSavedByModelcheck,
        options.exportStepBase,
        exportedStepMaybeExists,
        notes);
    std::exit(exitCode);
}

} // namespace

int main(int argc, char** argv) {
    const ProbeOptions options = options_from_args(argc, argv);
    std::vector<CallResult> calls;
    std::vector<std::string> notes;
    int modelcheckErrors = -1;
    int modelcheckWarnings = -1;
    ProBoolean modelSavedByModelcheck = PRO_B_FALSE;
    ProError validationScoreCode = PRO_TK_GENERAL_ERROR;
    std::string validationScore;

    const auto optionErrors = validate_options(options);
    if (!optionErrors.empty()) {
        write_result(
            options,
            "InvalidArguments",
            false,
            false,
            calls,
            validationScore,
            validationScoreCode,
            modelcheckErrors,
            modelcheckWarnings,
            modelSavedByModelcheck,
            options.exportStepBase,
            false,
            optionErrors);
        for (const auto& error : optionErrors) {
            std::cerr << error << '\n';
        }
        return 2;
    }

    std::filesystem::create_directories(options.outputDir);
    std::filesystem::create_directories(options.modelcheckOutputDir);

    ProError err = ProEngineerStart(
        const_cast<char*>(options.creoCommand.c_str()),
        const_cast<char*>(options.textPath.c_str()));
    calls.push_back({"ProEngineerStart", err});
    if (err != PRO_TK_NO_ERROR) {
        notes.push_back("Creo did not start through ProEngineerStart.");
        write_result(
            options,
            "CreoStartFailed",
            false,
            false,
            calls,
            validationScore,
            validationScoreCode,
            modelcheckErrors,
            modelcheckWarnings,
            modelSavedByModelcheck,
            options.exportStepBase,
            false,
            notes);
        return 3;
    }

    ProPath baseRemovedStepPath;
    ProPath geomagicPatchStepPath;
    ProPath modelcheckOutputDir;
    ProPath exportStepBase;
    ProMdlName modelName;
    if (!copy_to_pro_path(options.baseRemovedStep, baseRemovedStepPath) ||
        !copy_to_pro_path(options.geomagicPatchStep, geomagicPatchStepPath) ||
        !copy_to_pro_path(options.modelcheckOutputDir, modelcheckOutputDir) ||
        !copy_to_pro_path(options.exportStepBase, exportStepBase) ||
        !copy_to_model_name(options.modelName, modelName)) {
        notes.push_back("One or more paths exceeded Creo ProPath or ProMdlName limits. The wrapper stages inputs to avoid this.");
        write_and_end(
            options,
            "CreoPathConversionFailed",
            4,
            false,
            false,
            calls,
            validationScore,
            validationScoreCode,
            modelcheckErrors,
            modelcheckWarnings,
            modelSavedByModelcheck,
            notes,
            true);
    }

    ProMdl createdModel = nullptr;
    err = ProIntfimportModelWithOptionsMdlnameCreate(
        baseRemovedStepPath,
        nullptr,
        PRO_INTF_IMPORT_STEP,
        PRO_MDL_PART,
        PRO_IMPORTREP_MASTER,
        modelName,
        nullptr,
        nullptr,
        &createdModel);
    calls.push_back({"ProIntfimportModelWithOptionsMdlnameCreate(base_removed_candidate)", err});
    if (err != PRO_TK_NO_ERROR || createdModel == nullptr) {
        notes.push_back("Base-removed STEP import through Pro/TOOLKIT failed before patch import.");
        write_and_end(
            options,
            "BaseRemovedStepImportFailed",
            5,
            false,
            false,
            calls,
            validationScore,
            validationScoreCode,
            modelcheckErrors,
            modelcheckWarnings,
            modelSavedByModelcheck,
            notes,
            true);
    }

    ProIntfDataSource patchDataSource;
    err = ProIntfDataSourceInit(PRO_INTF_STEP, geomagicPatchStepPath, &patchDataSource);
    calls.push_back({"ProIntfDataSourceInit(geomagic_patch)", err});
    if (err != PRO_TK_NO_ERROR) {
        notes.push_back("Could not initialize the STEP data source for geomagic_patch.");
        write_and_end(
            options,
            "PatchDataSourceInitFailed",
            6,
            false,
            false,
            calls,
            validationScore,
            validationScoreCode,
            modelcheckErrors,
            modelcheckWarnings,
            modelSavedByModelcheck,
            notes,
            true);
    }

    ProImportfeatAttr attributes = {};
    attributes.join_surfaces = 1;
    attributes.attempt_make_solid = 1;
    attributes.cut_or_add = 0;
    attributes.add_bodies = 0;
    attributes.body_arr = nullptr;

    ProFeature importFeature;
    err = ProImportfeatCreate(
        reinterpret_cast<ProSolid>(createdModel),
        &patchDataSource,
        nullptr,
        &attributes,
        &importFeature);
    calls.push_back({"ProImportfeatCreate(geomagic_patch join_surfaces=1 attempt_make_solid=1)", err});
    const auto cleanErr = ProIntfDataSourceClean(&patchDataSource);
    calls.push_back({"ProIntfDataSourceClean(geomagic_patch)", cleanErr});
    if (err != PRO_TK_NO_ERROR) {
        notes.push_back("ProImportfeatCreate failed with join_surfaces=1 and attempt_make_solid=1.");
        write_and_end(
            options,
            "PatchImportFeatureCreateFailed",
            7,
            false,
            false,
            calls,
            validationScore,
            validationScoreCode,
            modelcheckErrors,
            modelcheckWarnings,
            modelSavedByModelcheck,
            notes,
            true);
    }

    err = ProSolidRegenerate(reinterpret_cast<ProSolid>(createdModel), PRO_REGEN_NO_FLAGS);
    calls.push_back({"ProSolidRegenerate", err});
    const bool regenOk = (err == PRO_TK_NO_ERROR);
    if (!regenOk) {
        notes.push_back("ProSolidRegenerate did not return PRO_TK_NO_ERROR after patch import feature creation.");
    }

    err = ProMdlSave(createdModel);
    calls.push_back({"ProMdlSave", err});
    const bool saveOk = (err == PRO_TK_NO_ERROR);
    if (!saveOk) {
        notes.push_back("ProMdlSave failed after regenerate.");
    }

    ProLine score;
    std::fill(score, score + PRO_LINE_SIZE, L'\0');
    validationScoreCode = ProIntfimportValidationscoreGet(createdModel, score);
    calls.push_back({"ProIntfimportValidationscoreGet", validationScoreCode});
    if (validationScoreCode != PRO_TK_NO_ERROR) {
        std::fill(score, score + PRO_LINE_SIZE, L'\0');
        validationScoreCode = ProIntfimportValidationscoreCalculate(createdModel, score);
        calls.push_back({"ProIntfimportValidationscoreCalculate", validationScoreCode});
    }
    validationScore = narrow_from_wide(score);

    err = ProModelcheckExecute(
        createdModel,
        PRO_B_FALSE,
        PRO_MODELCHECK_NO_GRAPHICS,
        nullptr,
        modelcheckOutputDir,
        &modelcheckErrors,
        &modelcheckWarnings,
        &modelSavedByModelcheck);
    calls.push_back({"ProModelcheckExecute", err});
    if (err != PRO_TK_NO_ERROR) {
        notes.push_back("ProModelcheckExecute failed. The import/export result is still recorded.");
    }

    err = ProIntf3DFileWriteWithDefaultProfile(
        reinterpret_cast<ProSolid>(createdModel),
        PRO_INTF_EXPORT_STEP,
        exportStepBase);
    calls.push_back({"ProIntf3DFileWriteWithDefaultProfile(PRO_INTF_EXPORT_STEP)", err});
    const bool exportOk = (err == PRO_TK_NO_ERROR);
    if (!exportOk) {
        notes.push_back("STEP export from the Creo-merged model failed.");
    }

    const bool validationLooksSolid =
        validationScore.empty() ? true : !contains_fail_signal(validationScore);
    if (!validationLooksSolid) {
        notes.push_back("Import validation score contains FAIL/SOLID_FAILED.");
    }
    if (modelcheckErrors > 0) {
        notes.push_back("ModelCHECK returned errors. SHORT_EDGES remains diagnostic-only, but GEOM_CHECKS/SOLID_FAILED are not acceptable for the gray-solid target.");
    }

    const bool stage1AcceptedByToolkit =
        regenOk &&
        saveOk &&
        exportOk &&
        validationLooksSolid;

    write_and_end(
        options,
        "CreoToolkitPhase1Completed",
        stage1AcceptedByToolkit ? 0 : 10,
        true,
        stage1AcceptedByToolkit,
        calls,
        validationScore,
        validationScoreCode,
        modelcheckErrors,
        modelcheckWarnings,
        modelSavedByModelcheck,
        notes,
        true);
}
