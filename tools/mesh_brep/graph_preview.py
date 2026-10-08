"""Source-UV tessellation preview of native supports, not STEP verification."""
import json
from pathlib import Path
import numpy as np
from .mesh import dump
from .partition_fit import parameters, positions


def preview(run, output):
    root = Path(run); out = Path(output); out.mkdir(parents=True, exist_ok=False)
    meta = json.loads((root/'run.json').read_text(encoding='utf-8')); data = np.load(meta['partition'])
    v, f = data['mesh_vertex_original'], data['mesh_triangle']
    points, cells, normals = [], [], []; offset = 0
    for path in sorted((root/'charts').glob('*.npz')):
        rec = dict(np.load(path)); used = rec['vertex_ids']; q = parameters(v[used], rec)
        p = positions(q, rec); n = np.cross(positions(q, rec, 1, 0), positions(q, rec, 0, 1))
        n /= np.linalg.norm(n, axis=1)[:, None]
        points.append(p); cells.append(np.searchsorted(used, f[rec['triangle_ids']])+offset)
        normals.append(n); offset += len(p)
    np.savez_compressed(out/'display_mesh.npz', points=np.concatenate(points), triangles=np.concatenate(cells),
                        normals=np.concatenate(normals))
    dump(out/'run.json', dict(partition=meta['partition'], source_fit=str(root.resolve()), status='preview_only',
        scope='Original source triangle UVs evaluated on fitted supports with native derivatives. Not STEP trim or watertightness verification.'))
