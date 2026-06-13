#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Headless CLI for the reference Global Cut Chain STL cutter.

This wrapper intentionally executes the pure algorithm slice from
scripts/cutter_global_chain_mode.py instead of importing that module. The
reference script imports PyQt5 and pythonocc at module import time for its GUI,
while this CLI only needs numpy/trimesh/rtree/triangle and the STL cutter code.
"""

from __future__ import annotations

import argparse
import hashlib
import heapq
import importlib.util
import json
import math
import os
import shutil
import sys
import time
import traceback
from collections import defaultdict, deque
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Set, Tuple


REQUIRED_MODULES = ("numpy", "trimesh", "rtree", "triangle")
REFERENCE_SCRIPT_NAME = "cutter_global_chain_mode.py"


def missing_modules() -> list[str]:
    return [name for name in REQUIRED_MODULES if importlib.util.find_spec(name) is None]


def load_numeric_points(path: Path, label: str) -> list[list[float]]:
    with path.open("r", encoding="utf-8") as stream:
        value = json.load(stream)

    if isinstance(value, dict):
        value = value.get("points", value.get(label, []))

    if not isinstance(value, list):
        raise RuntimeError(f"{label} JSON must contain a point list.")

    points: list[list[float]] = []
    for index, item in enumerate(value):
        if not isinstance(item, list) or len(item) != 3:
            raise RuntimeError(f"{label}[{index}] must be [x, y, z].")
        try:
            point = [float(item[0]), float(item[1]), float(item[2])]
        except Exception as exc:
            raise RuntimeError(f"{label}[{index}] contains a non-numeric value.") from exc
        if not all(math.isfinite(v) for v in point):
            raise RuntimeError(f"{label}[{index}] contains a non-finite value.")
        points.append(point)
    return points


def write_summary(path: Optional[Path], payload: dict) -> None:
    if path is None:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as stream:
        json.dump(payload, stream, ensure_ascii=False, indent=2)


def exec_reference_slice(reference_script: Path) -> dict:
    missing = missing_modules()
    if missing:
        raise RuntimeError(
            "Missing Python modules for Global Cut Chain: "
            + ", ".join(missing)
            + ". Run scripts/setup_global_chain_python.ps1."
        )

    import numpy as np
    import trimesh

    source = reference_script.read_text(encoding="utf-8")

    def slice_between(start_marker: str, end_marker: str) -> str:
        start = source.index(start_marker)
        end = source.index(end_marker, start)
        return source[start:end]

    namespace = {
        "__file__": str(reference_script),
        "__name__": "_spo_global_chain_reference_core",
        "os": os,
        "sys": sys,
        "math": math,
        "heapq": heapq,
        "hashlib": hashlib,
        "shutil": shutil,
        "Path": Path,
        "defaultdict": defaultdict,
        "deque": deque,
        "Iterable": Iterable,
        "List": List,
        "Optional": Optional,
        "Sequence": Sequence,
        "Set": Set,
        "Tuple": Tuple,
        "Dict": Dict,
        "np": np,
        "trimesh": trimesh,
    }

    exec(slice_between("# 参数区", "# OCC 工具"), namespace)
    exec(slice_between("# STL / 几何工具", "# Qt Worker"), namespace)
    return namespace


def run(args: argparse.Namespace) -> int:
    reference_script = Path(args.reference_script or Path(__file__).with_name(REFERENCE_SCRIPT_NAME))
    if not reference_script.exists():
        raise RuntimeError(f"Reference script does not exist: {reference_script}")

    namespace = exec_reference_slice(reference_script)
    boundary_points = load_numeric_points(Path(args.boundary_json), "boundary")
    seed_points = load_numeric_points(Path(args.seeds_json), "seeds") if args.seeds_json else []

    if len(boundary_points) < 3:
        raise RuntimeError("Boundary JSON contains fewer than 3 points.")

    output_path = Path(args.output)
    debug_dir = Path(args.debug_dir) if args.debug_dir else output_path.with_suffix("").parent / (output_path.stem + "_debug")

    if args.no_debug:
        namespace["WRITE_DEBUG"] = False

    stl_mesh = namespace["load_stl_mesh"](args.stl)
    result = namespace["cut_one_loop_global_chain"](
        stl_mesh=stl_mesh,
        red_loop_points=boundary_points,
        output_stl=str(output_path),
        seed_point=seed_points if seed_points else None,
        debug_dir=str(debug_dir),
        label=args.label,
    )

    import trimesh

    patch = trimesh.load(result, process=False, force="mesh")
    summary = {
        "success": True,
        "message": "Global Cut Chain CLI completed.",
        "output_stl": str(Path(result).resolve()),
        "triangle_count": int(len(patch.faces)),
        "vertex_count": int(len(patch.vertices)),
        "area": float(patch.area),
        "bounds": patch.bounds.tolist() if len(patch.vertices) else [],
    }
    write_summary(Path(args.summary_json) if args.summary_json else None, summary)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run the reference Global Cut Chain STL cutter without GUI/OCC imports.")
    parser.add_argument("--stl", required=False, help="Original source STL path.")
    parser.add_argument("--boundary-json", required=False, help="JSON file containing ordered boundary points.")
    parser.add_argument("--seeds-json", required=False, help="Optional JSON file containing seed points.")
    parser.add_argument("--output", required=False, help="Output patch STL path.")
    parser.add_argument("--summary-json", required=False, help="Optional JSON summary path.")
    parser.add_argument("--debug-dir", required=False, help="Optional debug artifact directory.")
    parser.add_argument("--reference-script", required=False, help="Override path to cutter_global_chain_mode.py.")
    parser.add_argument("--label", default="loop_0", help="Debug artifact label.")
    parser.add_argument("--no-debug", action="store_true", help="Disable reference debug artifact exports.")
    parser.add_argument("--self-test", action="store_true", help="Check dependencies and reference markers, then exit.")
    return parser


def main(argv: list[str]) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    started = time.perf_counter()
    summary_path = Path(args.summary_json) if args.summary_json else None

    try:
        reference_script = Path(args.reference_script or Path(__file__).with_name(REFERENCE_SCRIPT_NAME))
        if args.self_test:
            namespace = exec_reference_slice(reference_script)
            required = ("load_stl_mesh", "cut_one_loop_global_chain", "GlobalCutChainCutter")
            missing = [name for name in required if name not in namespace]
            if missing:
                raise RuntimeError("Reference slice is missing: " + ", ".join(missing))
            print("Global Cut Chain CLI self-test OK")
            return 0

        required_args = {
            "--stl": args.stl,
            "--boundary-json": args.boundary_json,
            "--output": args.output,
        }
        missing_cli = [name for name, value in required_args.items() if not value]
        if missing_cli:
            raise RuntimeError("Missing required arguments: " + ", ".join(missing_cli))

        code = run(args)
        elapsed = time.perf_counter() - started
        print(f"[DONE] global_chain_cut_cli elapsed={elapsed:.3f}s")
        return code
    except Exception as exc:
        payload = {
            "success": False,
            "message": str(exc),
            "elapsed_seconds": time.perf_counter() - started,
            "traceback": traceback.format_exc(),
        }
        write_summary(summary_path, payload)
        print("[ERROR]", exc, file=sys.stderr)
        traceback.print_exc()
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
