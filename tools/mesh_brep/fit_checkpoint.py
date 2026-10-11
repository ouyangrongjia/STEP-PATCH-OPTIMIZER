"""Compare cached geometry directly before reusing costly diagnostics."""
import json
from pathlib import Path
import shutil
from zipfile import BadZipFile
import numpy as np


CHART_FIELDS = ('origin', 'axes', 'lo', 'size', 'knots', 'height',
                'region', 'triangle_ids', 'vertex_ids')


def save(source, output):
    root, out = Path(source), Path(output); out.mkdir(parents=True, exist_ok=False)
    for name in ('charts', 'curves', 'routes'):
        if (root/name).exists():
            shutil.copytree(root/name, out/name)
    for name in ('run.json', 'chart_index.npz'):
        shutil.copy2(root/name, out/name)
    meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    shutil.copy2(meta['partition'], out/'source_partition.npz')


def same_arrays(left, right, fields=None):
    with np.load(left) as a, np.load(right) as b:
        keys = set(a.files) if fields is None else set(fields)
        if fields is None and keys != set(b.files):
            return False
        return (keys <= set(a.files) and keys <= set(b.files)
                and all(np.array_equal(a[k], b[k]) for k in keys))


def matches(source, snapshot):
    try:
        return _matches(source, snapshot)
    except (OSError, ValueError, KeyError, BadZipFile):
        return False


def _matches(source, snapshot):
    root, old = Path(source), Path(snapshot)
    if not (old/'source_partition.npz').exists():
        return False
    meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    prior = json.loads((old/'run.json').read_text(encoding='utf-8'))
    if meta['config'] != prior['config'] or not same_arrays(meta['partition'], old/'source_partition.npz'):
        return False
    if not same_arrays(root/'chart_index.npz', old/'chart_index.npz'):
        return False
    for name in ('charts', 'curves', 'routes'):
        paths = sorted((root/name).glob('*.npz')); saved = sorted((old/name).glob('*.npz'))
        if [p.name for p in paths] != [p.name for p in saved]:
            return False
        for a, b in zip(paths, saved):
            if not same_arrays(a, b, CHART_FIELDS if name == 'charts' else None):
                return False
    return True


def adopt_audit(source, stage):
    """Migrate a completed legacy audit after native-geometry readback.

Only immutable inputs older than the audit are eligible. Full native support
parameters and trim identity are checked again; later resumes use snapshots.
"""
    from . import cad
    from .distance_guard import match_faces
    root, folder = Path(source), Path(stage)
    paths = [folder/name/'run.json' for name in ('assembled','step','quality')]+[folder/'stratified.json']
    if not all(p.exists() for p in paths) or (folder/'input').exists():
        return False
    assembly, step, quality, dense = [json.loads(p.read_text(encoding='utf-8')) for p in paths]
    if (any(r.get('status')!='DiagnosticOnly' for r in (assembly,step,quality))
            or not step.get('watertight') or Path(assembly['source_fit']).resolve()!=root.resolve()):
        return False
    model=folder/'step/model.step'
    if (not model.exists() or model.stat().st_size!=step['step_bytes']
            or any(Path(r['step']).resolve()!=model.resolve() for r in (quality,dense))):
        return False
    meta=json.loads((root/'run.json').read_text(encoding='utf-8'))
    if any(Path(r['partition']).resolve()!=Path(meta['partition']).resolve() for r in (assembly,quality,dense)):
        return False
    inputs=[root/'run.json',root/'chart_index.npz',Path(meta['partition']),Path(meta['stl'])]
    inputs += [p for name in ('charts','curves','routes') for p in (root/name).glob('*.npz')]
    if any(p.stat().st_mtime_ns > min(x.stat().st_mtime_ns for x in paths) for p in inputs):
        return False
    records={int(p.stem):dict(np.load(p)) for p in (root/'charts').glob('*.npz')}
    mapping=match_faces(cad.read_generated(model),folder/'assembled',records)
    save(root,folder/'input')
    (folder/'input/adoption.json').write_text(json.dumps(dict(
        legacy_audit_adopted=True, native_supports_and_trim_identity_checked=len(mapping),
        source_inputs_older_than_audit=True, geometry_modified=False,
        scope='One-time migration of immutable legacy inputs; subsequent resumes compare stored arrays directly.'),indent=2)+'\n',encoding='utf-8')
    return True
