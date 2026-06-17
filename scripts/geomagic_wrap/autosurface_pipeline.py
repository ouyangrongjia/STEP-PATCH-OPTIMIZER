"""Convert an STL region to STP through Geomagic Wrap AutoSurface.

This script intentionally keeps the pipeline minimal:

    STL mesh
    -> optional RepairMesh / RemoveNonManifoldVertices / FillSmallHoles
    -> optional Remesh / QuickSmooth / Relax
    -> AutoSurface to temporary IGS
    -> ReadFile(IGS)
    -> WriteFile(STP)

It does not run Solidify or healCAD.

Default AutoSurface strategy:

    numPatches = 1
    autoMerge = True
    adaptiveFit = False

This matches the API constraint that autoMerge cannot be combined with adaptiveFit.

wrapCore.exe may not forward command-line arguments after --script to sys.argv.
When sys.argv is empty, pass parameters through environment variables instead:

    set "FIT_REGION_INPUT=data/crop_stl/model/candidate_0001.stl" && set "FIT_REGION_OUTPUT=data/crop_stp/model/candidate_0001.stp" && set "FIT_REGION_REPAIR_MESH=1" && wrapCore.exe --script fit_region.py

Main environment variables:
    FIT_REGION_INPUT
    FIT_REGION_OUTPUT
    FIT_REGION_REPAIR_MESH             default: 1
    FIT_REGION_FILL_HOLE_MAX_EDGES     default: 80
    FIT_REGION_FILL_HOLE_LENGTH_RATIO  default: 1.0
    FIT_REGION_KEEP_TEMP                default: 1
    FIT_REGION_SKIP_REMESH              default: 1
    FIT_REGION_QUICK_SMOOTH             default: 0
    FIT_REGION_RELAX                    default: 0
    FIT_REGION_AUTOSURFACE_TARGET       default: 1
    FIT_REGION_AUTOSURFACE_TOLERANCE    default: 0.03
    FIT_REGION_DETAIL_LEVEL             default: 0.10
    FIT_REGION_GEOMETRY_MODE            default: Mechanical
    FIT_REGION_ADAPTIVE_FIT             default: 0
    FIT_REGION_AUTO_MERGE               default: 1
    FIT_REGION_STRICT_PATCH_TARGET      default: 1
    FIT_REGION_SHARPEN_CONTOURS        default: 0
"""

import argparse
import os
import shutil
import sys
import time
import traceback
import uuid

_LAST_STAGE = "module loading"
_TEE_FILE = None
_ORIGINAL_STDOUT = sys.stdout
_ORIGINAL_STDERR = sys.stderr
_GEOMAGIC_API_IMPORTED = False

geo = None
ReadFile = None
Remesh = None
CalculateTargetEdgeLength = None
WriteFile = None
AutoSurface = None
QuickSmooth = None
Relax = None
RepairMesh = None
RepairStrategy = None
RemoveNonManifoldVertices = None
FillSmallHoles = None
Analyze = None


class PipelineExit(Exception):
    def __init__(self, code):
        Exception.__init__(self, "Pipeline exited with code {}".format(code))
        self.code = code


class TeeStream(object):
    def __init__(self, console_stream, file_stream):
        self.console_stream = console_stream
        self.file_stream = file_stream

    def write(self, message):
        if message is None:
            return
        try:
            self.console_stream.write(message)
            self.console_stream.flush()
        except Exception:
            pass
        try:
            self.file_stream.write(message)
            self.file_stream.flush()
        except Exception:
            pass

    def flush(self):
        try:
            self.console_stream.flush()
        except Exception:
            pass
        try:
            self.file_stream.flush()
        except Exception:
            pass


def script_dir():
    try:
        return os.path.dirname(os.path.abspath(__file__))
    except Exception:
        return os.getcwd()


def bootstrap_log_path():
    return os.path.join(script_dir(), "fit_region_bootstrap.log")


def bootstrap_write(message):
    try:
        with open(bootstrap_log_path(), "a") as fp:
            fp.write("[{}] {}\n".format(time.strftime("%Y-%m-%d %H:%M:%S"), message))
            fp.flush()
    except Exception:
        pass


bootstrap_write("module loaded")
bootstrap_write("__file__={}".format(globals().get("__file__", "<missing>")))
bootstrap_write("cwd={}".format(os.getcwd()))
bootstrap_write("argv={}".format(sys.argv))


def print_flush(message="", stream=None):
    if stream is None:
        stream = sys.stdout
    stream.write(str(message) + "\n")
    stream.flush()


def fatal(message, code):
    print_flush(message, stream=sys.stderr)
    raise PipelineExit(code)


def set_stage(stage):
    global _LAST_STAGE
    _LAST_STAGE = stage
    bootstrap_write("stage={}".format(stage))
    print_flush("\n=== {} ===".format(stage))


def env_bool(name, default=False):
    value = os.environ.get(name)
    if value is None or value == "":
        return default
    return value.strip().lower() in ("1", "true", "yes", "y", "on")


def env_float(name, default):
    value = os.environ.get(name)
    if value is None or value == "":
        return default
    return float(value)


def env_int(name, default):
    value = os.environ.get(name)
    if value is None or value == "":
        return default
    return int(value)


def parse_args():
    parser = argparse.ArgumentParser(description="AutoSurface an STL region and export as STP")
    parser.add_argument("-i", "--input", default=None)
    parser.add_argument("-o", "--output", default=None)
    parser.add_argument("-e", "--target-edge-length", type=float, default=None)
    parser.add_argument("-w", "--work-dir", default=None)
    parser.add_argument("--log-file", default=None)
    parser.add_argument("--keep-temp", action="store_true", default=False)
    parser.add_argument("--skip-remesh", action="store_true", default=False)
    parser.add_argument("--quick-smooth", action="store_true", default=False)
    parser.add_argument("--relax", action="store_true", default=False)
    parser.add_argument("--repair-mesh", dest="repair_mesh", action="store_true", default=None)
    parser.add_argument("--no-repair-mesh", dest="repair_mesh", action="store_false")
    parser.add_argument("--fill-hole-max-edges", type=int, default=None)
    parser.add_argument("--fill-hole-length-ratio", type=float, default=None)
    parser.add_argument("--relax-iteration", type=int, default=None)
    parser.add_argument("--relax-strength", type=float, default=None)
    parser.add_argument("--autosurface-target", type=int, default=None)
    parser.add_argument("--autosurface-tolerance", type=float, default=None)
    parser.add_argument("--detail-level", type=float, default=None)
    parser.add_argument("--geometry-mode", default=None)
    parser.add_argument("--adaptive-fit", action="store_true", default=False)
    parser.add_argument("--auto-merge", action="store_true", default=False)
    parser.add_argument("--strict-patch-target", action="store_true", default=False)
    parser.add_argument("--sharpen-contours", action="store_true", default=False)

    try:
        args, unknown = parser.parse_known_args(sys.argv[1:])
        if unknown:
            bootstrap_write("ignored unknown argv tokens: {}".format(unknown))
    except Exception:
        bootstrap_write("argparse failed; falling back to environment variables")
        args = parser.parse_args([])

    args.input = args.input or os.environ.get("FIT_REGION_INPUT")
    args.output = args.output or os.environ.get("FIT_REGION_OUTPUT")
    args.target_edge_length = args.target_edge_length if args.target_edge_length is not None else env_float("FIT_REGION_TARGET_EDGE_LENGTH", 0.0)
    args.work_dir = args.work_dir or os.environ.get("FIT_REGION_WORK_DIR")
    args.log_file = args.log_file or os.environ.get("FIT_REGION_LOG_FILE")
    args.keep_temp = args.keep_temp or env_bool("FIT_REGION_KEEP_TEMP", True)
    if args.repair_mesh is None:
        args.repair_mesh = env_bool("FIT_REGION_REPAIR_MESH", True)
    args.fill_hole_max_edges = args.fill_hole_max_edges if args.fill_hole_max_edges is not None else env_int("FIT_REGION_FILL_HOLE_MAX_EDGES", 80)
    args.fill_hole_length_ratio = args.fill_hole_length_ratio if args.fill_hole_length_ratio is not None else env_float("FIT_REGION_FILL_HOLE_LENGTH_RATIO", 1.0)

    # Remesh is experimental for small boundary crops; keep it opt-in.
    args.skip_remesh = args.skip_remesh or env_bool("FIT_REGION_SKIP_REMESH", True)
    args.quick_smooth = args.quick_smooth or env_bool("FIT_REGION_QUICK_SMOOTH", False)
    args.relax = args.relax or env_bool("FIT_REGION_RELAX", False)
    args.relax_iteration = args.relax_iteration if args.relax_iteration is not None else env_int("FIT_REGION_RELAX_ITERATION", 2)
    args.relax_strength = args.relax_strength if args.relax_strength is not None else env_float("FIT_REGION_RELAX_STRENGTH", 0.25)

    args.autosurface_target = args.autosurface_target if args.autosurface_target is not None else env_int("FIT_REGION_AUTOSURFACE_TARGET", 1)
    args.autosurface_tolerance = args.autosurface_tolerance if args.autosurface_tolerance is not None else env_float("FIT_REGION_AUTOSURFACE_TOLERANCE", 0.03)
    args.detail_level = args.detail_level if args.detail_level is not None else env_float("FIT_REGION_DETAIL_LEVEL", 0.10)
    args.geometry_mode = args.geometry_mode or os.environ.get("FIT_REGION_GEOMETRY_MODE", "Mechanical")
    args.adaptive_fit = args.adaptive_fit or env_bool("FIT_REGION_ADAPTIVE_FIT", False)
    args.auto_merge = args.auto_merge or env_bool("FIT_REGION_AUTO_MERGE", True)
    args.strict_patch_target = args.strict_patch_target or env_bool("FIT_REGION_STRICT_PATCH_TARGET", True)
    args.sharpen_contours = args.sharpen_contours or env_bool("FIT_REGION_SHARPEN_CONTOURS", False)

    if args.auto_merge and args.adaptive_fit:
        # Geomagic API explicitly disallows autoMerge combined with adaptiveFit.
        bootstrap_write("autoMerge=True forces adaptiveFit=False")
        args.adaptive_fit = False

    bootstrap_write("resolved args input={} output={} log_file={}".format(args.input, args.output, args.log_file))
    return args


def default_log_file(args):
    if args.log_file:
        return os.path.abspath(args.log_file)
    if args.output:
        output_abs = os.path.abspath(args.output)
        output_dir = os.path.dirname(output_abs)
        output_base = os.path.splitext(os.path.basename(output_abs))[0]
        return os.path.join(output_dir, output_base + "_fit_region.log")
    if args.input:
        input_abs = os.path.abspath(args.input)
        input_dir = os.path.dirname(input_abs)
        input_base = os.path.splitext(os.path.basename(input_abs))[0]
        return os.path.join(input_dir, input_base + "_fit_region.log")
    return os.path.join(script_dir(), "fit_region.log")


def setup_diagnostics(log_file):
    global _TEE_FILE
    log_dir = os.path.dirname(os.path.abspath(log_file))
    if log_dir and not os.path.isdir(log_dir):
        os.makedirs(log_dir)
    _TEE_FILE = open(log_file, "w", encoding="utf-8-sig")
    sys.stdout = TeeStream(_ORIGINAL_STDOUT, _TEE_FILE)
    sys.stderr = TeeStream(_ORIGINAL_STDERR, _TEE_FILE)
    print_flush("fit_region.py diagnostic log")
    print_flush("Timestamp: {}".format(time.strftime("%Y-%m-%d %H:%M:%S")))
    print_flush("Log file: {}".format(os.path.abspath(log_file)))
    print_flush("Bootstrap log file: {}".format(bootstrap_log_path()))
    print_flush("Python executable: {}".format(sys.executable))
    print_flush("Python version: {}".format(sys.version.replace("\n", " ")))
    print_flush("Current working directory: {}".format(os.getcwd()))
    print_flush("argv: {}".format(sys.argv))
    print_flush("FIT_REGION_INPUT: {}".format(os.environ.get("FIT_REGION_INPUT")))
    print_flush("FIT_REGION_OUTPUT: {}".format(os.environ.get("FIT_REGION_OUTPUT")))
    print_flush("FIT_REGION_REPAIR_MESH: {}".format(os.environ.get("FIT_REGION_REPAIR_MESH")))
    print_flush("FIT_REGION_FILL_HOLE_MAX_EDGES: {}".format(os.environ.get("FIT_REGION_FILL_HOLE_MAX_EDGES")))
    print_flush("FIT_REGION_FILL_HOLE_LENGTH_RATIO: {}".format(os.environ.get("FIT_REGION_FILL_HOLE_LENGTH_RATIO")))
    print_flush("FIT_REGION_AUTO_MERGE: {}".format(os.environ.get("FIT_REGION_AUTO_MERGE")))
    print_flush("FIT_REGION_ADAPTIVE_FIT: {}".format(os.environ.get("FIT_REGION_ADAPTIVE_FIT")))
    print_flush("")


def close_diagnostics():
    global _TEE_FILE
    try:
        sys.stdout.flush()
        sys.stderr.flush()
    except Exception:
        pass
    sys.stdout = _ORIGINAL_STDOUT
    sys.stderr = _ORIGINAL_STDERR
    if _TEE_FILE is not None:
        try:
            _TEE_FILE.close()
        except Exception:
            pass
        _TEE_FILE = None


def get_attr_safe(obj, attr, default=0):
    try:
        val = getattr(obj, attr, default)
        if callable(val):
            val = val()
        return val
    except Exception:
        return default


def validate_args(args):
    if not args.input:
        fatal("Error: missing input. Use -i/--input or FIT_REGION_INPUT.", 10)
    if not args.output:
        fatal("Error: missing output. Use -o/--output or FIT_REGION_OUTPUT.", 11)
    args.input = os.path.abspath(args.input)
    args.output = os.path.abspath(args.output)
    if args.work_dir is None:
        args.work_dir = os.path.dirname(args.input)
    args.work_dir = os.path.abspath(args.work_dir)
    if not os.path.isfile(args.input):
        fatal("Error: input file not found: {}".format(args.input), 1)
    if not os.path.isdir(args.work_dir):
        fatal("Error: work directory not found: {}".format(args.work_dir), 12)
    output_dir = os.path.dirname(args.output)
    if output_dir and not os.path.isdir(output_dir):
        fatal("Error: output directory not found: {}".format(output_dir), 13)
    if args.autosurface_target <= 0:
        fatal("Error: AutoSurface target must be positive: {}".format(args.autosurface_target), 15)
    if args.autosurface_tolerance <= 0.0:
        fatal("Error: AutoSurface tolerance must be positive: {}".format(args.autosurface_tolerance), 16)
    if args.detail_level < 0.0 or args.detail_level > 1.0:
        fatal("Error: detail level must be in [0, 1]: {}".format(args.detail_level), 18)
    if args.fill_hole_max_edges < 0:
        fatal("Error: fill hole max edges must be non-negative: {}".format(args.fill_hole_max_edges), 19)
    if args.fill_hole_length_ratio < 0.0:
        fatal("Error: fill hole length ratio must be non-negative: {}".format(args.fill_hole_length_ratio), 21)


def safe_ascii_temp_igs(input_path):
    temp_root = os.path.join(script_dir(), "fit_region_temp")
    if not os.path.isdir(temp_root):
        os.makedirs(temp_root)
    base = os.path.splitext(os.path.basename(input_path))[0]
    try:
        safe_base = base.encode("ascii", "ignore").decode("ascii") or "region"
    except Exception:
        safe_base = "region"
    return os.path.join(temp_root, safe_base + "_" + uuid.uuid4().hex[:8] + ".igs")


def desired_igs_path(output_path):
    output_abs = os.path.abspath(output_path)
    output_dir = os.path.dirname(output_abs)
    output_base = os.path.splitext(os.path.basename(output_abs))[0]
    return os.path.join(output_dir, output_base + "_autosurface.igs")


def print_args(args, temp_igs_path, final_igs_path, log_file):
    print_flush("Resolved arguments:")
    for name in [
        "input", "output", "target_edge_length", "repair_mesh", "fill_hole_max_edges",
        "fill_hole_length_ratio", "skip_remesh", "quick_smooth", "relax",
        "relax_iteration", "relax_strength", "autosurface_target", "autosurface_tolerance",
        "detail_level", "geometry_mode", "adaptive_fit", "auto_merge", "strict_patch_target",
        "sharpen_contours",
        "work_dir", "keep_temp"
    ]:
        print_flush("  {}: {}".format(name, getattr(args, name)))
    print_flush("  temp_iges_ascii: {}".format(temp_igs_path))
    print_flush("  saved_iges: {}".format(final_igs_path))
    print_flush("  log_file: {}".format(log_file))
    print_flush("  note: Solidify and healCAD are disabled")
    print_flush("  note: mesh repair targets small holes, non-manifold edges, and non-manifold vertices")
    print_flush("  note: autoMerge=True forces adaptiveFit=False")
    print_flush("")


def apply_mm_file_open_options(reader):
    try:
        options = geo.FileOpenOptions()
        try:
            options.units = geo.Length.Millimeters
        except Exception:
            pass
        reader.options = options
        print_flush("  FileOpenOptions units set to millimeters")
        return True
    except Exception as exc:
        print_flush("  Warning: failed to set FileOpenOptions units: {}".format(exc))
        return False


def ensure_geomagic_api_imported():
    global _GEOMAGIC_API_IMPORTED
    global geo, ReadFile, Remesh, CalculateTargetEdgeLength, WriteFile, AutoSurface, QuickSmooth, Relax
    global RepairMesh, RepairStrategy, RemoveNonManifoldVertices, FillSmallHoles, Analyze
    if _GEOMAGIC_API_IMPORTED:
        return
    set_stage("Importing Geomagic API")
    try:
        import geomagic.api.v3 as _geo
        from geomagic.api.v3 import ReadFile as _ReadFile
        from geomagic.api.v3 import Remesh as _Remesh
        from geomagic.api.v3 import CalculateTargetEdgeLength as _CalculateTargetEdgeLength
        from geomagic.api.v3 import WriteFile as _WriteFile
        from geomagic.api.v3 import AutoSurface as _AutoSurface
        from geomagic.api.v3 import QuickSmooth as _QuickSmooth
        from geomagic.api.v3 import Relax as _Relax
        from geomagic.api.v3 import RepairMesh as _RepairMesh
        from geomagic.api.v3 import RepairStrategy as _RepairStrategy
        from geomagic.api.v3 import RemoveNonManifoldVertices as _RemoveNonManifoldVertices
        from geomagic.api.v3 import FillSmallHoles as _FillSmallHoles
        from geomagic.api.v3 import Analyze as _Analyze
    except Exception:
        print_flush("Error: failed to import geomagic.api.v3", stream=sys.stderr)
        traceback.print_exc(file=sys.stderr)
        raise PipelineExit(20)
    geo = _geo
    ReadFile = _ReadFile
    Remesh = _Remesh
    CalculateTargetEdgeLength = _CalculateTargetEdgeLength
    WriteFile = _WriteFile
    AutoSurface = _AutoSurface
    QuickSmooth = _QuickSmooth
    Relax = _Relax
    RepairMesh = _RepairMesh
    RepairStrategy = _RepairStrategy
    RemoveNonManifoldVertices = _RemoveNonManifoldVertices
    FillSmallHoles = _FillSmallHoles
    Analyze = _Analyze
    _GEOMAGIC_API_IMPORTED = True
    print_flush("  Geomagic API imported successfully.")


def step_import_stl(filename):
    reader = ReadFile()
    reader.filename = filename
    reader.type = "stl"
    apply_mm_file_open_options(reader)
    reader.run()
    mesh = getattr(reader, "mesh", None)
    if mesh is None:
        fatal("Error: failed to import mesh from {}".format(filename), 2)
    return mesh


def describe_mesh_health(mesh, label):
    try:
        analyzer = Analyze()
        analyzer.mesh = mesh
        analyzer.run()
        print_flush(
            "  {}: manifold={}, open={}, openEdges={}, boundaryCycles={}, nonManifoldEdges={}, "
            "nonManifoldVertices={}, degenerateTriangles={}, components={}, maxHoleLength={}".format(
                label,
                get_attr_safe(analyzer, "isManifold", "?"),
                get_attr_safe(analyzer, "isOpen", "?"),
                get_attr_safe(analyzer, "numOpenEdges", "?"),
                get_attr_safe(analyzer, "numBoundaryCycles", "?"),
                get_attr_safe(analyzer, "numNonManifoldEdges", "?"),
                get_attr_safe(analyzer, "numNonManifoldVertices", "?"),
                get_attr_safe(analyzer, "numDegenerateTriangles", "?"),
                get_attr_safe(analyzer, "numComponents", "?"),
                get_attr_safe(analyzer, "maxHoleLength", "?"),
            )
        )
    except Exception as exc:
        print_flush("  Warning: Analyze failed for {}: {}".format(label, exc), stream=sys.stderr)


def mesh_max_hole_length(mesh):
    try:
        analyzer = Analyze()
        analyzer.mesh = mesh
        analyzer.run()
        return float(get_attr_safe(analyzer, "maxHoleLength", 0.0) or 0.0)
    except Exception as exc:
        print_flush("  Warning: Analyze failed while reading maxHoleLength: {}".format(exc), stream=sys.stderr)
        return 0.0


def step_fill_small_holes(mesh, max_edges, length_ratio):
    if max_edges <= 0 or length_ratio <= 0.0:
        print_flush("  FillSmallHoles disabled by threshold")
        return mesh
    max_hole_length = mesh_max_hole_length(mesh)
    if max_hole_length <= 0.0:
        print_flush("  FillSmallHoles skipped: maxHoleLength is 0")
        return mesh
    try:
        limit = max_hole_length * float(length_ratio)
        filler = FillSmallHoles()
        filler.mesh = mesh
        filler.maxNumEdges = int(max_edges)
        filler.maxHoleLength = limit
        filler.run()
        mesh = getattr(filler, "mesh", mesh)
        print_flush(
            "  FillSmallHoles finished. maxNumEdges={}, maxHoleLength={:.9f}, numFilled={}, triangles={}".format(
                int(max_edges),
                limit,
                get_attr_safe(filler, "numFilled", 0),
                get_attr_safe(mesh, "numTriangles", 0),
            )
        )
    except Exception as exc:
        print_flush("  Warning: FillSmallHoles failed and was skipped: {}".format(exc), stream=sys.stderr)
    return mesh


def step_repair_mesh(mesh, args):
    describe_mesh_health(mesh, "Before RepairMesh")
    try:
        strategy = RepairStrategy()
        for name in ["spikeVertices", "smallComponents", "smallTunnels", "intersections", "spikeEdges"]:
            try:
                setattr(strategy, name, False)
            except Exception:
                pass
        strategy.smallHoles = True
        strategy.nonManifoldEdges = True

        repair = RepairMesh()
        repair.mesh = mesh
        repair.strategy = strategy
        try:
            repair.forceUpdate = True
        except Exception:
            pass

        repair.run()
        mesh = getattr(repair, "mesh", mesh)
        print_flush(
            "  RepairMesh finished. smallHoles={}, nonManifoldEdges={}, totalProblems={}, triangles={}".format(
                get_attr_safe(repair, "numSmallHoles", 0),
                get_attr_safe(repair, "numNonManifoldEdges", 0),
                get_attr_safe(repair, "totalProblems", 0),
                get_attr_safe(mesh, "numTriangles", 0),
            )
        )
    except Exception as exc:
        print_flush("  Warning: RepairMesh failed and was skipped: {}".format(exc), stream=sys.stderr)
    try:
        remover = RemoveNonManifoldVertices()
        remover.mesh = mesh
        try:
            remover.globalAlgorithm = True
        except Exception:
            pass
        remover.run()
        mesh = getattr(remover, "mesh", mesh)
        print_flush("  RemoveNonManifoldVertices finished. triangles={}".format(get_attr_safe(mesh, "numTriangles", 0)))
    except Exception as exc:
        print_flush("  Warning: RemoveNonManifoldVertices failed and was skipped: {}".format(exc), stream=sys.stderr)
    mesh = step_fill_small_holes(mesh, args.fill_hole_max_edges, args.fill_hole_length_ratio)
    describe_mesh_health(mesh, "After RepairMesh")
    return mesh


def step_remesh(mesh, target_edge_length):
    try:
        if target_edge_length <= 0.0:
            calc = CalculateTargetEdgeLength()
            calc.mesh = mesh
            calc.run()
            target_edge_length = calc.targetEdgeLength
            print_flush("  Auto-calculated target edge length: {:.6f}".format(target_edge_length))
        remesh = Remesh()
        remesh.mesh = mesh
        remesh.targetEdgeLength = target_edge_length
        remesh.run()
        return getattr(remesh, "mesh", mesh)
    except Exception as exc:
        print_flush("  Warning: Remesh failed and original mesh will be used: {}".format(exc), stream=sys.stderr)
        return mesh


def step_quick_smooth(mesh):
    try:
        smoother = QuickSmooth()
        smoother.mesh = mesh
        smoother.run()
        mesh = getattr(smoother, "mesh", mesh)
        print_flush("  QuickSmooth finished. triangles={}".format(get_attr_safe(mesh, "numTriangles", 0)))
    except Exception as exc:
        print_flush("  Warning: QuickSmooth failed and was skipped: {}".format(exc), stream=sys.stderr)
    return mesh


def step_relax(mesh, iteration, strength):
    try:
        relaxer = Relax()
        relaxer.mesh = mesh
        try:
            relaxer.iterations = iteration
        except Exception:
            pass
        try:
            relaxer.strength = strength
        except Exception:
            pass
        try:
            relaxer.fixBoundaries = True
        except Exception:
            pass
        relaxer.run()
        mesh = getattr(relaxer, "mesh", mesh)
        print_flush("  Relax finished. triangles={}".format(get_attr_safe(mesh, "numTriangles", 0)))
    except Exception as exc:
        print_flush("  Warning: Relax failed and was skipped: {}".format(exc), stream=sys.stderr)
    return mesh


def set_autosurface_geometry(autosurf, mode):
    try:
        mode_norm = (mode or "Mechanical").strip().lower()
        if mode_norm == "mechanical":
            autosurf.geometry = geo.AutoSurface.Mechanical
            return "Mechanical"
        autosurf.geometry = geo.AutoSurface.Organic
        return "Organic"
    except Exception as exc:
        print_flush("  Warning: failed to set AutoSurface.geometry: {}".format(exc))
        return mode


def remove_if_exists(path):
    try:
        if os.path.isfile(path):
            os.remove(path)
            print_flush("  Removed stale output: {}".format(path))
    except Exception:
        pass


def run_autosurface_once(mesh, igs_path, geometry_mode, adaptive_fit, num_patches, tolerance, detail_level, auto_merge, sharpen_contours, label):
    remove_if_exists(igs_path)
    print_flush("  [AutoSurface attempt] {}".format(label))
    try:
        if auto_merge and adaptive_fit:
            print_flush("    autoMerge=True requested with adaptiveFit=True; forcing adaptiveFit=False")
            adaptive_fit = False

        autosurf = AutoSurface()
        autosurf.mesh = mesh
        autosurf.fileName = igs_path
        geom = set_autosurface_geometry(autosurf, geometry_mode)
        autosurf.tolerance = float(tolerance)
        autosurf.autoMerge = bool(auto_merge)
        autosurf.adaptiveFit = bool(adaptive_fit)
        try:
            autosurf.detail = float(detail_level)
        except Exception as exc:
            print_flush("    Warning: failed to set detail: {}".format(exc))
        try:
            autosurf.duplicateMesh = False
        except Exception:
            pass
        try:
            autosurf.extendContours = False
        except Exception:
            pass
        try:
            autosurf.sharpenConstrainedContours = bool(sharpen_contours)
        except Exception as exc:
            print_flush("    Warning: failed to set sharpenConstrainedContours: {}".format(exc))
        if num_patches is not None:
            autosurf.numPatches = int(num_patches)

        print_flush("    geometry={}, tolerance={}, detail={}, adaptiveFit={}, autoMerge={}, numPatches={}, sharpenContours={}".format(
            geom, tolerance, detail_level, adaptive_fit, auto_merge, num_patches, sharpen_contours
        ))
        t0 = time.time()
        autosurf.run()
        elapsed = time.time() - t0
        err = get_attr_safe(autosurf, "errorMsg", "")
        if err:
            print_flush("    AutoSurface.errorMsg: {}".format(err), stream=sys.stderr)
        if os.path.isfile(igs_path):
            print_flush("    success: IGES created ({:.1f}s): {} KB".format(elapsed, os.path.getsize(igs_path) / 1024.0))
            return True, ""
        return False, err or "IGES not created"
    except Exception as exc:
        print_flush("    exception: {}".format(exc), stream=sys.stderr)
        return False, str(exc)


def build_autosurface_attempts(args):
    target = int(args.autosurface_target)
    geom = args.geometry_mode
    tol = float(args.autosurface_tolerance)
    detail = float(args.detail_level)
    attempts = []

    def add(label, geometry, adaptive, patches, tolerance, detail_level, auto_merge):
        key = (geometry, bool(adaptive), patches, float(tolerance), float(detail_level), bool(auto_merge))
        for existing in attempts:
            if existing[0] == key:
                return
        attempts.append((key, label, geometry, adaptive, patches, tolerance, detail_level, auto_merge))

    requested_auto_merge = bool(args.auto_merge)

    # First: the requested one-patch strategy.
    add("requested autoMerge={}".format(requested_auto_merge), geom, False, target, tol, detail, requested_auto_merge)

    # Then keep numPatches=1 but vary detail/geometry/tolerance before relaxing patch count.
    add("one-patch autoMerge detail=0.0", geom, False, target, tol, 0.0, True)
    add("one-patch autoMerge Mechanical", "Mechanical", False, target, tol, detail, True)
    add("one-patch autoMerge larger tolerance", geom, False, target, max(tol, 0.08), detail, True)
    add("one-patch no autoMerge", geom, False, target, tol, detail, False)

    if not args.strict_patch_target:
        for patches in [2, 4, 8]:
            if patches != target:
                add("fallback patches={} autoMerge".format(patches), geom, False, patches, tol, detail, True)
        add("fallback automatic numPatches autoMerge", geom, False, 0, tol, detail, True)
        add("fallback automatic numPatches no autoMerge", geom, False, 0, tol, detail, False)

    return attempts


def log_autosurface_attrs():
    try:
        sample = AutoSurface()
        attrs = [name for name in dir(sample) if not name.startswith("_")]
        print_flush("  AutoSurface attrs: {}".format([
            name for name in attrs
            if any(k in name.lower() for k in ("mesh", "file", "patch", "geometry", "tolerance", "merge", "fit", "detail", "error", "duplicate", "contour"))
        ]))
    except Exception:
        pass


def run_autosurface(mesh, igs_path, args):
    log_autosurface_attrs()
    errors = []
    for _, label, geometry, adaptive, patches, tolerance, detail, auto_merge in build_autosurface_attempts(args):
        ok, err = run_autosurface_once(mesh, igs_path, geometry, adaptive, patches, tolerance, detail, auto_merge, args.sharpen_contours, label)
        if ok:
            return True
        errors.append("{} => {}".format(label, err))

    fatal("Error: all AutoSurface attempts failed: " + " | ".join(errors), 16)


def try_run_autosurface_to_step(mesh, temp_igs_path, final_igs_path, args):
    log_autosurface_attrs()
    errors = []
    saw_step_write_failure = False

    for _, label, geometry, adaptive, patches, tolerance, detail, auto_merge in build_autosurface_attempts(args):
        set_stage("Step 5/7: AutoSurface to IGES")
        ok, err = run_autosurface_once(mesh, temp_igs_path, geometry, adaptive, patches, tolerance, detail, auto_merge, args.sharpen_contours, label)
        if not ok:
            errors.append("{} => {}".format(label, err))
            continue

        success, bodies, loops, err = convert_igs_to_stp_plain(temp_igs_path, args.output)
        if success:
            if args.keep_temp:
                preserve_igs(temp_igs_path, final_igs_path)
            return True, bodies, loops, "", 0

        saw_step_write_failure = True
        print_flush(
            "  STEP write failed for this AutoSurface attempt: openLoops={}, error={}".format(loops, err),
            stream=sys.stderr,
        )
        errors.append("{} => STEP write failed (openLoops={}): {}".format(label, loops, err))

    exit_code = 17 if saw_step_write_failure else 16
    return False, 0, 0, "Error: all AutoSurface to STEP attempts failed: " + " | ".join(errors), exit_code


def run_autosurface_to_step(mesh, temp_igs_path, final_igs_path, args):
    ok, bodies, loops, error, exit_code = try_run_autosurface_to_step(
        mesh, temp_igs_path, final_igs_path, args)
    if ok:
        return bodies, loops
    fatal(error, exit_code)


def convert_igs_to_stp_plain(igs_path, stp_path):
    set_stage("Step 6/7: IGS to STEP via ReadFile + WriteFile")
    bodies = 0
    loops = 0
    try:
        remove_if_exists(stp_path)
        reader = ReadFile()
        reader.filename = igs_path
        apply_mm_file_open_options(reader)
        reader.run()
        cad_model = getattr(reader, "cadModel", None)
        if cad_model is None:
            return False, 0, 0, "ReadFile cadModel is None"
        bodies = get_attr_safe(cad_model, "numBodies", 0)
        loops = get_attr_safe(cad_model, "numCADOpenEdgeLoops", 0)
        print_flush("  cadModel: bodies={}, openLoops={}".format(bodies, loops))

        writer = WriteFile()
        writer.cadModel = cad_model
        writer.filename = stp_path
        try:
            writer.filterId = 5  # STEP214, harmless if unsupported
        except Exception:
            pass
        writer.run()
        if os.path.exists(stp_path):
            print_flush("  STEP written: {} KB".format(os.path.getsize(stp_path) / 1024.0))
            return True, bodies, loops, ""
        return False, bodies, loops, "WriteFile did not create STEP"
    except Exception as exc:
        print_flush("  plain IGS to STEP failed: {}".format(exc), stream=sys.stderr)
        return False, bodies, loops, str(exc)


def preserve_igs(temp_igs, final_igs):
    if not os.path.isfile(temp_igs):
        return None
    try:
        final_dir = os.path.dirname(os.path.abspath(final_igs))
        if final_dir and not os.path.isdir(final_dir):
            os.makedirs(final_dir)
        shutil.copy2(temp_igs, final_igs)
        print_flush("  Preserved IGES: {}".format(final_igs))
        return final_igs
    except Exception as exc:
        print_flush("  Warning: failed to copy IGES to output directory: {}".format(exc), stream=sys.stderr)
        print_flush("  IGES remains at ASCII temp path: {}".format(temp_igs))
        return temp_igs


def run_pipeline(args, temp_igs_path, final_igs_path):
    ensure_geomagic_api_imported()
    set_stage("Step 1/7: Importing STL")
    mesh = step_import_stl(args.input)
    print_flush("  Imported mesh: {} triangles, {} points".format(get_attr_safe(mesh, "numTriangles", 0), get_attr_safe(mesh, "numPoints", 0)))

    set_stage("Step 2/7: Optional mesh repair")
    if args.repair_mesh:
        mesh = step_repair_mesh(mesh, args)
    else:
        print_flush("  RepairMesh disabled")

    pre_remesh_mesh = mesh
    remesh_attempted = False
    if args.skip_remesh:
        set_stage("Step 3/7: Skipping Remesh")
        print_flush("  Remesh skipped")
    else:
        set_stage("Step 3/7: Remeshing")
        remesh_attempted = True
        mesh = step_remesh(mesh, args.target_edge_length)
        print_flush("  Remeshed: {} triangles".format(get_attr_safe(mesh, "numTriangles", 0)))

    set_stage("Step 4/7: Optional mesh smoothing")
    if args.quick_smooth:
        mesh = step_quick_smooth(mesh)
    else:
        print_flush("  QuickSmooth disabled")
    if args.relax and args.relax_iteration > 0:
        mesh = step_relax(mesh, args.relax_iteration, args.relax_strength)
    else:
        print_flush("  Relax disabled")

    ok, bodies, loops, error, exit_code = try_run_autosurface_to_step(
        mesh, temp_igs_path, final_igs_path, args)
    if not ok and remesh_attempted:
        print_flush(
            "  Warning: AutoSurface failed after Remesh; retrying with pre-remesh mesh.",
            stream=sys.stderr,
        )
        fallback_ok, fallback_bodies, fallback_loops, fallback_error, fallback_exit_code = try_run_autosurface_to_step(
            pre_remesh_mesh, temp_igs_path, final_igs_path, args)
        if fallback_ok:
            bodies, loops = fallback_bodies, fallback_loops
            print_flush("  AutoSurface fallback with pre-remesh mesh succeeded.")
        else:
            fatal(error + " | pre-remesh fallback => " + fallback_error, fallback_exit_code or exit_code)
    elif ok:
        pass
    else:
        fatal(error, exit_code)

    set_stage("Step 7/7: Done")
    print_flush("  Saved STEP: {}".format(args.output))
    print_flush("  bodies={}, openLoops={}".format(bodies, loops))


def main():
    args = parse_args()
    log_file = default_log_file(args)
    setup_diagnostics(log_file)
    validate_args(args)
    temp_igs_path = safe_ascii_temp_igs(args.input)
    final_igs_path = desired_igs_path(args.output)
    remove_if_exists(args.output)
    remove_if_exists(final_igs_path)
    print_args(args, temp_igs_path, final_igs_path, log_file)
    try:
        run_pipeline(args, temp_igs_path, final_igs_path)
    finally:
        if os.path.isfile(temp_igs_path):
            if args.keep_temp:
                print_flush("Keeping ASCII temporary IGES: {}".format(temp_igs_path))
            else:
                try:
                    os.remove(temp_igs_path)
                    print_flush("Removed temporary IGES: {}".format(temp_igs_path))
                except Exception as exc:
                    print_flush("Warning: failed to remove temporary IGES: {}".format(exc), stream=sys.stderr)


if __name__ == "__main__":
    exit_code = 0
    try:
        main()
        print_flush("\nPipeline finished successfully.")
    except PipelineExit as exc:
        exit_code = exc.code
        print_flush("\nPipeline exited with code {} at stage: {}".format(exit_code, _LAST_STAGE), stream=sys.stderr)
    except Exception:
        exit_code = 99
        print_flush("\nUnhandled exception at stage: {}".format(_LAST_STAGE), stream=sys.stderr)
        traceback.print_exc(file=sys.stderr)
    finally:
        try:
            print_flush("Final exit code: {}".format(exit_code))
        except Exception:
            pass
        bootstrap_write("final exit code={} stage={}".format(exit_code, _LAST_STAGE))
        close_diagnostics()
