# Geomagic AutoSurface Pipeline

`autosurface_pipeline.py` is the T4.3 Geomagic-side script copied from the verified `fit_region.py` flow with light integration changes only. It keeps the existing AutoSurface fallback strategy and exports both IGES and STEP.

Official crop path rule:

```text
data/crop_stl/<relative>/<name>.stl -> data/crop_stp/<relative>/<name>.stp
data/crop_stl/<relative>/<name>.stl -> data/crop_igs/<relative>/<name>.igs
```

Manual run from the repository root:

```powershell
$env:FIT_REGION_INPUT = "data/crop_stl/example/candidate_0001.stl"
$env:FIT_REGION_OUTPUT = "data/crop_stp/example/candidate_0001.stp"
$env:FIT_REGION_OUTPUT_IGES = "data/crop_igs/example/candidate_0001.igs"
$env:FIT_REGION_RESULT_JSON = "data/geomagic_work/example/autosurface_result.json"
$env:FIT_REGION_LOG_FILE = "data/geomagic_work/example/fit_region.log"
$env:FIT_REGION_WORK_DIR = "data/geomagic_work/example"
$env:FIT_REGION_CONFIG_JSON = "data/geomagic_work/example/autosurface_config.json"
$env:FIT_REGION_STRICT_PATCH_TARGET = "0"
& "E:\Geomagic Wrap\wrapCore.exe" --script "scripts/geomagic_wrap/autosurface_pipeline.py"
```

`FIT_REGION_STRICT_PATCH_TARGET=0` enables the script fallback attempts after the one-patch target. The default path is still one-patch AutoSurface with `autoMerge=True` and `adaptiveFit=False`; if both flags are requested, the script keeps the Geomagic API-safe behavior and forces `adaptiveFit=False`.

Required environment variables:

```text
FIT_REGION_INPUT
FIT_REGION_OUTPUT
FIT_REGION_OUTPUT_IGES
```

Optional integration variables:

```text
FIT_REGION_RESULT_JSON
FIT_REGION_LOG_FILE
FIT_REGION_WORK_DIR
FIT_REGION_CONFIG_JSON
FIT_REGION_KEEP_TEMP
FIT_REGION_SKIP_REMESH
FIT_REGION_QUICK_SMOOTH
FIT_REGION_RELAX
FIT_REGION_AUTOSURFACE_TARGET
FIT_REGION_AUTOSURFACE_TOLERANCE
FIT_REGION_DETAIL_LEVEL
FIT_REGION_GEOMETRY_MODE
FIT_REGION_ADAPTIVE_FIT
FIT_REGION_AUTO_MERGE
FIT_REGION_STRICT_PATCH_TARGET
```

The result JSON is written on success and best-effort failure. It includes `success`, `timed_out`, `exit_code`, `bodies`, `open_loops`, `message`, `error_message`, `failed_stage`, `input_stl_path`, `output_iges_path`, `output_step_path`, `preserved_iges_path`, `config_json_path`, `result_json_path`, `fit_region_log_path`, and `duration_ms`.

Scope notes:

- This task does not implement GUI launch, QProcess orchestration, patch import, overlay, Apply, replacement, or topology gates.
- 不做 final CAD replacement.
- 自动测试不调用真实 Geomagic.
