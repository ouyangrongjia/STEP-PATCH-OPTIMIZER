#include <ProCore.h>
#include <ProIntf3Dexport.h>
#include <ProIntfimport.h>
#include <ProMdl.h>
#include <ProMdlChk.h>
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
    std::string stepPath;
    std::string creoCommand;
    std::string textPath;
    std::string outputDir;
    std::string modelcheckOutputDir;
    std::string exportStepBase;
    std::string resultJson;
    std::string modelName = "spo_tk_baseline";
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
                out << "\\u";
                out << std::hex << std::uppercase << static_cast<int>(static_cast<unsigned char>(c));
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

    options.stepPath = get("step");
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
    if (options.stepPath.empty()) {
        errors.push_back("--step is required.");
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
    if (!options.stepPath.empty() && !std::filesystem::exists(options.stepPath)) {
        errors.push_back("--step does not exist: " + options.stepPath);
    }
    return errors;
}

void write_result(
    const ProbeOptions& options,
    const std::string& status,
    const bool runnerSuccess,
    const bool stage0Accepted,
    const std::vector<CallResult>& calls,
    const std::string& validationScore,
    const ProError validationScoreCode,
    const int modelcheckErrors,
    const int modelcheckWarnings,
    const ProBoolean modelSaved,
    const std::string& exportedStepBase,
    const bool exportedStepMaybeExists,
    const std::vector<std::string>& notes) {
    std::filesystem::create_directories(std::filesystem::path(options.resultJson).parent_path());
    std::ofstream out(options.resultJson, std::ios::binary);
    out << "{\n";
    out << "  \"tool\": \"creo_toolkit_baseline_probe\",\n";
    out << "  \"status\": " << quote_json(status) << ",\n";
    out << "  \"runner_success\": " << bool_json(runnerSuccess) << ",\n";
    out << "  \"stage0_acceptance_passed\": " << bool_json(stage0Accepted) << ",\n";
    out << "  \"acceptance_rule\": \"Stage 0 treats SHORT_EDGES as diagnostic-only; import validation and Creo solid/export evidence are the primary baseline signals.\",\n";
    out << "  \"short_edges_diagnostic_only\": true,\n";
    out << "  \"known_modelcheck_item_names\": [\"GEOM_CHECKS\", \"SHORT_EDGES\", \"IMPORT_FEAT\", \"PARAM_INFO\"],\n";
    out << "  \"step_input\": " << quote_json(options.stepPath) << ",\n";
    out << "  \"output_dir\": " << quote_json(options.outputDir) << ",\n";
    out << "  \"modelcheck_output_dir\": " << quote_json(options.modelcheckOutputDir) << ",\n";
    out << "  \"export_step_base\": " << quote_json(exportedStepBase) << ",\n";
    out << "  \"export_step_maybe_exists\": " << bool_json(exportedStepMaybeExists) << ",\n";
    out << "  \"import_validation_score_get_code\": " << validationScoreCode << ",\n";
    out << "  \"import_validation_score\": " << quote_json(validationScore) << ",\n";
    out << "  \"modelcheck_errors\": " << modelcheckErrors << ",\n";
    out << "  \"modelcheck_warnings\": " << modelcheckWarnings << ",\n";
    out << "  \"modelcheck_model_saved\": " << bool_json(modelSaved == PRO_B_TRUE) << ",\n";
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

bool contains_fail_signal(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value.find("FAIL") != std::string::npos ||
        value.find("SOLID_FAILED") != std::string::npos;
}

} // namespace

int main(int argc, char** argv) {
    const ProbeOptions options = options_from_args(argc, argv);
    std::vector<CallResult> calls;
    std::vector<std::string> notes;
    int modelcheckErrors = -1;
    int modelcheckWarnings = -1;
    ProBoolean modelSaved = PRO_B_FALSE;
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
            modelSaved,
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
            modelSaved,
            options.exportStepBase,
            false,
            notes);
        return 3;
    }

    ProMdl createdModel = nullptr;
    ProPath stepPath;
    ProPath modelcheckOutputDir;
    ProPath exportStepBase;
    ProMdlName modelName;
    if (!copy_to_pro_path(options.stepPath, stepPath) ||
        !copy_to_pro_path(options.modelcheckOutputDir, modelcheckOutputDir) ||
        !copy_to_pro_path(options.exportStepBase, exportStepBase) ||
        !copy_to_model_name(options.modelName, modelName)) {
        notes.push_back("One or more paths exceeded Creo ProPath or ProMdlName limits. The wrapper stages inputs to avoid this.");
        ProEngineerEnd();
        write_result(
            options,
            "CreoPathConversionFailed",
            false,
            false,
            calls,
            validationScore,
            validationScoreCode,
            modelcheckErrors,
            modelcheckWarnings,
            modelSaved,
            options.exportStepBase,
            false,
            notes);
        return 4;
    }

    err = ProIntfimportModelWithOptionsMdlnameCreate(
        stepPath,
        nullptr,
        PRO_INTF_IMPORT_STEP,
        PRO_MDL_PART,
        PRO_IMPORTREP_MASTER,
        modelName,
        nullptr,
        nullptr,
        &createdModel);
    calls.push_back({"ProIntfimportModelWithOptionsMdlnameCreate", err});
    if (err != PRO_TK_NO_ERROR || createdModel == nullptr) {
        notes.push_back("STEP import through Pro/TOOLKIT failed before ModelCHECK.");
        ProEngineerEnd();
        write_result(
            options,
            "CreoToolkitStepImportFailed",
            false,
            false,
            calls,
            validationScore,
            validationScoreCode,
            modelcheckErrors,
            modelcheckWarnings,
            modelSaved,
            options.exportStepBase,
            false,
            notes);
        return 5;
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
        &modelSaved);
    calls.push_back({"ProModelcheckExecute", err});
    if (err != PRO_TK_NO_ERROR) {
        notes.push_back("ProModelcheckExecute failed. The import result is still recorded.");
    }

    err = ProIntf3DFileWriteWithDefaultProfile(
        reinterpret_cast<ProSolid>(createdModel),
        PRO_INTF_EXPORT_STEP,
        exportStepBase);
    calls.push_back({"ProIntf3DFileWriteWithDefaultProfile", err});
    const bool exportCalledOk = (err == PRO_TK_NO_ERROR);
    const bool exportMaybeExists =
        exportCalledOk &&
        (std::filesystem::exists(options.exportStepBase + ".stp") ||
         std::filesystem::exists(options.exportStepBase + ".step"));
    if (!exportCalledOk) {
        notes.push_back("STEP export from the Toolkit-imported model failed.");
    }

    const bool importLooksSolid = !validationScore.empty() && !contains_fail_signal(validationScore);
    const bool stage0Accepted = importLooksSolid && exportCalledOk;
    if (!importLooksSolid) {
        notes.push_back("Import validation score is empty or contains FAIL/SOLID_FAILED; this does not meet the gray-solid baseline target.");
    }
    if (modelcheckErrors > 0) {
        notes.push_back("ModelCHECK returned errors. For stage 0, SHORT_EDGES remains diagnostic-only; inspect generated ModelCHECK reports to distinguish GEOM_CHECKS from SHORT_EDGES.");
    }

    ProEngineerEnd();
    calls.push_back({"ProEngineerEnd", PRO_TK_NO_ERROR});

    write_result(
        options,
        "CreoToolkitBaselineReady",
        true,
        stage0Accepted,
        calls,
        validationScore,
        validationScoreCode,
        modelcheckErrors,
        modelcheckWarnings,
        modelSaved,
        options.exportStepBase,
        exportMaybeExists,
        notes);

    return stage0Accepted ? 0 : 10;
}
