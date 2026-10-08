"""Sparse Loop limit stencils and mesh-only geometric fitting.

Smooth vertices use the original Loop beta; declared crease chains use cubic
B-spline boundary masks. Partition labels are inherited by every child face.
The same global linear stencil defines both sides of every regional interface.
"""
import numpy as np
from scipy import sparse
from scipy.sparse.linalg import factorized

from .mesh import Locator, stats


def connectivity(f):
    raw = f[:, [[0, 1], [1, 2], [2, 0]]].reshape(-1, 2)
    e, inverse, counts = np.unique(np.sort(raw, axis=1), axis=0, return_inverse=True, return_counts=True)
    if np.any(counts != 2):
        raise ValueError('Subdivision input must be a closed manifold')
    uses = np.argsort(inverse, kind='stable').reshape(-1, 2)
    opposite = f[uses//3, (uses % 3+2) % 3]
    return e, inverse.reshape(-1, 3), opposite


def even_mask(n, e, sharp, limit=False):
    valence = np.bincount(e.ravel(), minlength=n)
    beta = (5/8-(3/8+np.cos(2*np.pi/np.maximum(valence, 1))/4)**2)/np.maximum(valence, 1)
    if limit:
        beta = 1/(np.maximum(valence, 1)+3/(8*np.maximum(beta, 1e-30)))
    crease_edges = e[sharp]; degree = np.bincount(crease_edges.ravel(), minlength=n)
    corner = (degree > 0) & (degree != 2)
    diagonal = 1-valence*beta; diagonal[degree == 2] = 2/3 if limit else .75
    diagonal[corner] = 1
    rows = e.ravel(); cols = e[:, ::-1].ravel(); values = beta[rows]
    values[degree[rows] != 0] = 0
    cr = crease_edges.ravel(); cc = crease_edges[:, ::-1].ravel()
    cv = np.where(degree[cr] == 2, 1/6 if limit else .125, 0.)
    return sparse.coo_matrix((np.r_[diagonal, values, cv],
                              (np.r_[np.arange(n), rows, cr], np.r_[np.arange(n), cols, cc])), shape=(n, n)).tocsr()


def refine(n, f, sharp_keys, face_uv):
    e, inv, opposite = connectivity(f); m = len(e)
    sharp = np.array([tuple(x) in sharp_keys for x in e])
    even = even_mask(n, e, sharp)
    row = np.repeat(np.arange(m), 4); col = np.c_[e, opposite].ravel()
    weights = np.tile([.375, .375, .125, .125], (m, 1)); weights[sharp] = [.5, .5, 0., 0.]
    odd = sparse.coo_matrix((weights.ravel(), (row, col)), shape=(m, n)).tocsr()
    stencil = sparse.vstack([even, odd], format='csr')
    a, b, c = f.T; ab, bc, ca = (n+inv).T
    faces = np.stack([np.c_[a, ab, ca], np.c_[b, bc, ab], np.c_[c, ca, bc], np.c_[ab, bc, ca]], axis=1).reshape(-1, 3)
    u, w, z = face_uv.transpose(1, 0, 2)
    uw, wz, zu = (u+w)/2, (w+z)/2, (z+u)/2
    uv = np.stack([np.stack([u, uw, zu], axis=1), np.stack([w, wz, uw], axis=1),
                   np.stack([z, zu, wz], axis=1), np.stack([uw, wz, zu], axis=1)], axis=1).reshape(-1, 3, 2)
    new_sharp = {tuple(sorted((int(a), int(n+i)))) for i, (a, b) in enumerate(e) if sharp[i]}
    new_sharp.update(tuple(sorted((int(b), int(n+i)))) for i, (a, b) in enumerate(e) if sharp[i])
    return stencil, faces, new_sharp, uv


def limit_stencil(v, f, sharp_keys, levels=3):
    stencil = sparse.eye(len(v), format='csr'); faces = f.copy()
    uv = np.tile(np.array([[0., 0.], [1., 0.], [0., 1.]])[None], (len(f), 1, 1))
    parents = np.arange(len(f)); sharp = set(sharp_keys)
    for _ in range(levels):
        step, faces, sharp, uv = refine(stencil.shape[0], faces, sharp, uv)
        stencil = step@stencil; parents = np.repeat(parents, 4)
    e, _, _ = connectivity(faces)
    mask = np.array([tuple(x) in sharp for x in e])
    return even_mask(stencil.shape[0], e, mask, True)@stencil, faces, parents, uv


def fit_control_mesh(v, f, sharp, source_v, source_f, levels=3, iterations=5, regularization=.001):
    from .intersections import self_intersections
    import json
    sharp=set(sharp); safeguards=[]
    for attempt in range(16):
        stencil, fine_f, parents, uv = limit_stencil(v, f, sharp, levels)
        bad=self_intersections(stencil@v,fine_f)
        safeguards.append(dict(stage='initial_limit',attempt=attempt,self_intersecting_faces=int(len(bad)),crease_edges=len(sharp)))
        print(json.dumps(safeguards[-1]),flush=True)
        if not len(bad):break
        affected=np.unique(parents[bad]); before=len(sharp)
        vertices=np.unique(f[affected])
        local=f[np.isin(f,vertices).any(axis=1)]
        for tri in local:
            sharp.update(tuple(sorted((int(a),int(b)))) for a,b in zip(tri,np.roll(tri,-1)))
        if len(sharp)==before:raise ValueError('Unable to make a non-self-intersecting subdivision target')
    if len(bad):raise ValueError(f'Subdivision collision retry budget exceeded: {safeguards}')
    reference = Locator(source_v, source_f)
    p = stencil@v; cross = np.cross(p[fine_f[:, 1]]-p[fine_f[:, 0]], p[fine_f[:, 2]]-p[fine_f[:, 0]])
    area = np.linalg.norm(cross, axis=1); weights = np.zeros(len(p))
    for k in range(3):
        np.add.at(weights, fine_f[:, k], area/3)
    weights /= max(weights.mean(), 1e-30)
    # A weak displacement prior suppresses inverse-subdivision noise at tiny
    # fixed-label slivers without preventing the main shape from being fitted.
    h = (stencil.T@sparse.diags(weights)@stencil+sparse.eye(len(v))*regularization).tocsc()
    solve = factorized(h)
    control = v.copy(); history = []
    for i in range(iterations):
        p = stencil@control; target, distance, _, _ = reference.closest(p)
        delta = solve(stencil.T@(weights[:, None]*(target-p))-regularization*(control-v))
        trials=[];restricted=delta.copy();locked=np.zeros(len(v),dtype=bool)
        # Conflicts around a tiny fixed-label sliver must not freeze fitting on
        # the other regions. Freeze every control contributing to a conflicting
        # sample, then retry the same global update against the full mesh.
        for attempt in range(16):
            candidate=control+.8*restricted
            collisions=self_intersections(stencil@candidate,fine_f)
            trials.append(dict(attempt=attempt,step=.8,self_intersecting_faces=int(len(collisions)),locked_controls=int(locked.sum())))
            if not len(collisions):
                control=candidate;step=.8;break
            involved=np.unique(fine_f[collisions])
            locked[np.unique(stencil[involved].indices)]=True
            restricted[locked]=0
        else:step=0.
        history.append(dict(iteration=i, sampled_distance_mm=stats(distance), maximum_control_update_mm=float(np.linalg.norm(delta, axis=1).max()),collision_line_search=trials,accepted_step=step))
        if step==0:break
    return control, stencil@control, fine_f, parents, uv, history, sharp, safeguards
