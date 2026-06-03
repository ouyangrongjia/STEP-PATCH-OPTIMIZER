# Geomagic AutoSurface Pipeline

`autosurface_pipeline.py` is the Geomagic-side script copied from the verified `fit_region.py` flow. It keeps the AutoSurface fallback strategy, writes one diagnostic log, and exports STEP. When temp files are kept, the intermediate IGES is preserved beside the STEP as `<name>_autosurface.igs`.

Official crop path rule:

```text
data/crop_stl/<relative>/<name>.stl -> data/crop_stp/<relative>/<name>.stp
```

Manual run from `cmd.exe`:

```cmd
set "FIT_REGION_INPUT=D:\pyProject\step-patch-optimizer\data\crop_stl\example\candidate_0001.stl" && set "FIT_REGION_OUTPUT=D:\pyProject\step-patch-optimizer\data\crop_stp\example\candidate_0001.stp" && set "FIT_REGION_STRICT_PATCH_TARGET=0" && "E:\Geomagic Wrap\wrapCore.exe" --script "D:\pyProject\step-patch-optimizer\scripts\geomagic_wrap\autosurface_pipeline.py"
```

The backend follows the same minimal environment protocol: `FIT_REGION_INPUT`, `FIT_REGION_OUTPUT`, and `FIT_REGION_STRICT_PATCH_TARGET`. The script derives `<output>_fit_region.log` automatically. `FIT_REGION_REPAIR_MESH=1` is the script default and enables the pre-AutoSurface repair step for small holes, non-manifold edges, and non-manifold vertices. `FIT_REGION_STRICT_PATCH_TARGET=0` enables the script fallback attempts after the one-patch target. The default path is one-patch Mechanical AutoSurface with `autoMerge=True` and `adaptiveFit=False`; if both flags are requested, the script keeps the Geomagic API-safe behavior and forces `adaptiveFit=False`. `numPatches=1` is Geomagic's approximate AutoSurface target, not a guarantee that the exported STEP will contain one B-rep face.

Required environment variables:

```text
FIT_REGION_INPUT
FIT_REGION_OUTPUT
```

Optional variables:

```text
FIT_REGION_LOG_FILE
FIT_REGION_WORK_DIR
FIT_REGION_REPAIR_MESH
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

The script does not write result JSON. Diagnostics go to `FIT_REGION_LOG_FILE`, or to `<output>_fit_region.log` when that variable is omitted.

Scope notes:

- This task does not implement GUI launch, QProcess orchestration, patch import, overlay, Apply, replacement, or topology gates.
- 不做 final CAD replacement.
- 自动测试不调用真实 Geomagic.
