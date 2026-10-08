"""Fair complete label interfaces before subdividing them into shared trims.

Only source mesh geometry and the supplied labels enter this module. Artificial
chart junctions inherit points on a complete interface, instead of pinning a
new spline to each vertex of the old rasterized boundary.
"""
import numpy as np
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path
from scipy import sparse
from scipy.interpolate import BSpline
from .mesh import chain_parameters
from .partition_cad import shared_chains
from .splines import knots, sparse_basis


def _fit_task(task):
    return fair_curve(*task)


def subcurve(poles, knot, first, last):
    """Exact cubic restriction, including reversed parameter direction."""
    reverse = last < first
    a, b = sorted((float(first), float(last)))
    if b-a <= 1e-14:
        raise ValueError('Empty common-boundary interval')
    spline = BSpline(knot, poles, 3)
    for value in (a, b):
        multiplicity = np.count_nonzero(np.isclose(spline.t, value, rtol=0, atol=1e-14))
        if multiplicity < 4:
            spline = spline.insert_knot(value, 4-multiplicity)
    begin = int(np.flatnonzero(np.isclose(spline.t, a, rtol=0, atol=1e-14))[0])
    end = int(np.flatnonzero(np.isclose(spline.t, b, rtol=0, atol=1e-14))[-1])+1
    t = (spline.t[begin:end]-a)/(b-a)
    cp = spline.c[begin:begin+len(t)-4].copy()
    if reverse:
        cp, t = cp[::-1], 1-t[::-1]
    return cp, t


def fair_curve(points, parameters, budget, normals, normal_budget=.003, endpoints=None,
               tangent_locked=False, positive_only=False):
    """Minimum-bending cubic subject to sampled tube and plane constraints.

    Continuous extrema are checked separately, and violating parameters are
    inserted as constraints. Solver failure never certifies a geometric pass.
    """
    import osqp
    from .skeleton_graph import fit_boundary, normal_error
    from .regional_rectangle import curve_polyline_error
    points = np.asarray(points); parameters = np.asarray(parameters)
    ends = points[[0, -1]] if endpoints is None else np.asarray(endpoints)
    origin = points.mean(axis=0)
    length = max(float(np.linalg.norm(np.ptp(points, axis=0))), 1e-8)
    source = (points-origin)/length; anchors = (ends-origin)/length
    probe = np.unique(np.r_[parameters, (parameters[:-1]+parameters[1:])/2])
    for count in (() if positive_only else (6, 10, 18, 34, 66, 130)):
        t = knots(count)
        for enrich in range(4):
            target = np.column_stack([np.interp(probe, parameters, source[:, k]) for k in range(3)])
            segment = np.clip(np.searchsorted(parameters, probe, side='right')-1, 0, len(parameters)-2)
            b = sparse_basis(t, probe)
            nodes, weights = np.polynomial.legendre.leggauss(3)
            spans = np.unique(t); widths = np.diff(spans)
            q = (spans[:-1, None]+widths[:, None]*(nodes+1)/2).ravel()
            d2 = sparse_basis(t, q, 2).multiply(np.sqrt((widths[:, None]*weights/2).ravel())[:, None])
            h = (d2.T@d2+b.T@b*(.01/len(probe))+sparse.eye(count)*1e-9).tocsc()
            objective = sparse.block_diag([h]*3, format='csc')
            cost_scale = 10/max(float(h.diagonal().max()), 1.)
            linear = np.concatenate([-(b.T@target[:, k])*.01/len(probe) for k in range(3)])
            blocks = [sparse.block_diag([b]*3, format='csr')]
            lo = [target.T.ravel()-budget/(np.sqrt(3)*length)]
            hi = [target.T.ravel()+budget/(np.sqrt(3)*length)]
            # At a mesh vertex both limiting planes apply. Constraining only
            # the right segment makes continuous checking repeatedly rediscover
            # the same already-sampled vertex, without repairing its left limit.
            previous = np.clip(np.searchsorted(parameters, probe, side='left')-1, 0, len(parameters)-2)
            incident = np.concatenate([normals[segment], normals[previous]], axis=1)
            for side in range(incident.shape[1]):
                nn = incident[:, side]
                blocks.append(sparse.hstack([b.multiply(nn[:, k, None]) for k in range(3)], format='csr'))
                z = np.einsum('ij,ij->i', nn, target)
                lo.append(z-normal_budget*.9/length); hi.append(z+normal_budget*.9/length)
            endpoint = sparse_basis(t, np.array([0., 1.]))
            blocks.append(sparse.block_diag([endpoint]*3, format='csr'))
            lo.append(anchors.T.ravel()); hi.append(anchors.T.ravel())
            derivative = sparse_basis(t, np.array([0., 1.]), 1)
            direction = points[[1, -1]]-points[[0, -2]]
            direction /= np.linalg.norm(direction, axis=1)[:, None]
            blocks.append(sparse.hstack([derivative.multiply(direction[:, k, None]) for k in range(3)], format='csr'))
            lo.append(np.full(2, 1e-6)); hi.append(np.full(2, np.inf))
            if tangent_locked:
                seed = np.eye(3)[np.argmin(abs(direction), axis=1)]
                normal = np.cross(direction, seed); normal /= np.linalg.norm(normal, axis=1)[:, None]
                for side in (normal, np.cross(direction, normal)):
                    blocks.append(sparse.hstack([derivative.multiply(side[:, k, None]) for k in range(3)], format='csr'))
                    lo.append(np.zeros(2)); hi.append(np.zeros(2))
            solver = osqp.OSQP()
            solver.setup(P=objective*cost_scale, q=linear*cost_scale, A=sparse.vstack(blocks, format='csc'),
                         l=np.concatenate(lo), u=np.concatenate(hi), verbose=False,
                         eps_abs=2e-7, eps_rel=2e-7, max_iter=15000,
                         adaptive_rho_interval=50, polishing=True)
            solved = solver.solve(raise_error=False)
            if solved.info.status_val not in (1, 2):
                break
            cp = solved.x.reshape(3, count).T*length+origin
            # One canonical endpoint coordinate is used by all incident trims.
            cp[[0, -1]] = ends
            error, where = curve_polyline_error(cp, t, points, parameters)
            plane, plane_where = normal_error(cp, t, points, parameters, normals)
            if error <= budget*(1+1e-6) and plane <= normal_budget*(1+1e-6):
                return cp, t, error, dict(method='minimum_bending', source_plane_error_mm=plane)
            probe = np.unique(np.r_[probe, where, plane_where])
    # An explicit fallback remains reviewable; it is never silently reported
    # as a successful minimum-bending solve.
    guide = points.copy()
    guide += (1-parameters[:, None])*(ends[0]-points[0])+parameters[:, None]*(ends[1]-points[-1])
    shift = guide-points
    plane_shift = float(abs(np.einsum('eik,ejk->eij', np.stack([shift[:-1], shift[1:]], axis=1), normals)).max())
    distance_left = budget-float(np.linalg.norm(shift, axis=1).max())
    normal_left = normal_budget-plane_shift
    # The guide already spends part of both budgets by moving its anchors.
    # Spending the full normal budget again would force endless tightening.
    cp, t, _ = fit_boundary(guide, parameters, max(distance_left*.9, budget*.01),
                           normals, max(normal_left*.9, normal_budget*.01))
    error, _ = curve_polyline_error(cp, t, points, parameters)
    plane, _ = normal_error(cp, t, points, parameters, normals)
    return cp, t, error, dict(method='positive_topology_guard' if positive_only else 'positive_fallback',
                              source_plane_error_mm=plane)


class FeatureBoundaryNetwork:
    """Shared parent curves survive arbitrary splits inside the fixed labels."""
    def __init__(self, vertices, triangles, labels, edges, owners, smoothing, normal_budget,
                 workers=1, cache=None, complete=True):
        self.v, self.f = vertices, triangles
        self.smoothing, self.normal_budget = smoothing, normal_budget
        self.workers = workers; self.cache = None if cache is None else Path(cache)
        if self.cache is not None:
            self.cache.mkdir(parents=True, exist_ok=True)
        self.chains, self.pairs = shared_chains(edges, owners, labels) if complete else ([], [])
        fn = np.cross(vertices[triangles[:, 1]]-vertices[triangles[:, 0]],
                      vertices[triangles[:, 2]]-vertices[triangles[:, 0]])
        fn /= np.linalg.norm(fn, axis=1)[:, None]
        edge_keys = edges[:, 0]*len(vertices)+edges[:, 1]
        self.source = []; self.lookup = {}; self.parent = {}; self.scale = {}; self.internal = {}
        self.vertex_parents = {}
        self.internal_cache = {}; self.tangent_locked = set(); self.force_positive = set(); self.exact_cache = {}
        for ci, chain in enumerate(self.chains):
            keys = np.minimum(chain[:-1], chain[1:])*len(vertices)+np.maximum(chain[:-1], chain[1:])
            normals = fn[owners[np.searchsorted(edge_keys, keys)]]
            t = chain_parameters(vertices, chain)
            self.source.append((t, normals)); self.scale[ci] = 1.
            for vi in chain:
                self.vertex_parents.setdefault(int(vi), set()).add(ci)
            for a, b in zip(chain[:-1], chain[1:]):
                self.lookup[tuple(sorted((int(a), int(b))))] = ci
        self.refit(range(len(self.chains)))

    def refit(self, selected):
        tasks, pending = [], []
        for ci in sorted(selected):
            chain = self.chains[ci]; t, nn = self.source[ci]
            budget = self.smoothing*self.scale[ci]
            path = None if self.cache is None else self.cache/f'{ci:05d}.npz'
            if path is not None and path.exists():
                with np.load(path) as old:
                    if (int(old['version']) == 5 and float(old['budget']) == budget
                        and float(old['normal_budget']) == self.normal_budget
                        and np.array_equal(old['points'], self.v[chain])
                        and np.array_equal(old['normals'], nn)):
                        self.parent[ci] = (old['poles'], old['knots'], float(old['error']),
                            dict(method=str(old['method']), source_plane_error_mm=float(old['source_plane_error_mm'])))
                        continue
            pending.append(ci); tasks.append((self.v[chain], t, budget, nn, self.normal_budget))
        if not pending:
            return
        with ProcessPoolExecutor(max_workers=self.workers) as pool:
            for done, (ci, result) in enumerate(zip(pending, pool.map(_fit_task, tasks))):
                cp, kt, error, info = result
                self.parent[ci] = result
                if self.cache is not None:
                    np.savez_compressed(self.cache/f'{ci:05d}.npz', version=5, points=self.v[self.chains[ci]],
                        normals=self.source[ci][1], budget=self.smoothing*self.scale[ci], normal_budget=self.normal_budget,
                        poles=cp, knots=kt, error=error, **info)
                if done % 100 == 0 or done+1 == len(pending):
                    print(dict(stage='complete_interface', done=done+1, total=len(pending)), flush=True)

    def split_chains(self, chains, pairs):
        # A closed interface has canonical breaks even when the chart pair
        # does not change there. Respect those breaks before restricting it.
        result, owners = [], []
        for chain, pair in zip(chains, pairs):
            parent = [self.lookup.get(tuple(sorted(map(int, edge))), -1)
                      for edge in zip(chain[:-1], chain[1:])]
            cuts = np.r_[0, np.flatnonzero(np.diff(parent))+1, len(chain)-1]
            for first, last in zip(cuts[:-1], cuts[1:]):
                result.append(chain[first:last+1]); owners.append(pair)
        return result, owners

    def build(self, chains, normals):
        from .regional_rectangle import curve_polyline_error
        anchors = {}
        for ci, chain in enumerate(self.chains):
            cp, kt, _, _ = self.parent[ci]
            xyz = BSpline(kt, cp, 3)(self.source[ci][0])
            for vi, point in zip(chain, xyz):
                if int(vi) in anchors and np.linalg.norm(anchors[int(vi)]-point) > 1e-8:
                    raise ValueError('Inconsistent complete-interface junction')
                anchors[int(vi)] = point
        pending, tasks = [], []
        for chain, nn in zip(chains, normals):
            if tuple(sorted(map(int, chain[:2]))) in self.lookup:
                continue
            ends = np.array([anchors.get(int(vi), self.v[vi]) for vi in chain[[0, -1]]])
            signature = tuple(map(int, chain)); scale = self.internal.get(signature, 1.)
            locked = signature in self.tangent_locked
            positive = signature in self.force_positive
            cached = self.internal_cache.get(signature)
            if (cached is not None and cached[0] == scale and np.array_equal(cached[1], ends)
                and cached[2] == locked and cached[3] == positive):
                continue
            pending.append((signature, scale, ends, locked, positive))
            tasks.append((self.v[chain], chain_parameters(self.v, chain), self.smoothing*scale,
                          nn, self.normal_budget, ends, locked, positive))
        if pending:
            with ProcessPoolExecutor(max_workers=self.workers) as pool:
                for done, ((signature, scale, ends, locked, positive), fitted) in enumerate(zip(pending, pool.map(_fit_task, tasks))):
                    self.internal_cache[signature] = scale, ends.copy(), locked, positive, fitted
                    if done % 250 == 0 or done+1 == len(pending):
                        print(dict(stage='internal_interface', done=done+1, total=len(pending)), flush=True)
        result, samples, errors = [], [], []
        for ci, (chain, nn) in enumerate(zip(chains, normals)):
            key = tuple(sorted(map(int, chain[:2]))); parent = self.lookup.get(key)
            t = chain_parameters(self.v, chain)
            if parent is not None:
                source_chain = self.chains[parent]; pt = self.source[parent][0]
                index = {int(v): i for i, v in enumerate(source_chain)}
                start, end = pt[index[int(chain[0])]], pt[index[int(chain[-1])]]
                # All source edges must belong to this same supplied interface.
                if any(self.lookup.get(tuple(sorted(map(int, pair)))) != parent for pair in zip(chain[:-1], chain[1:])):
                    raise ValueError('Chart trim crosses complete label interfaces')
                cp, kt = subcurve(*self.parent[parent][:2], start, end)
                info = self.parent[parent][3]
            else:
                ends = np.array([anchors.get(int(vi), self.v[vi]) for vi in chain[[0, -1]]])
                signature = tuple(map(int, chain)); scale = self.internal.get(signature, 1.)
                cp, kt, _, info = self.internal_cache[signature][4]
            error, _ = curve_polyline_error(cp, kt, self.v[chain], t)
            rec = dict(poles=cp, knots=kt, chain=chain, parameters=t, movement_mm=error,
                       parent_interface=-1 if parent is None else parent,
                       movement_budget_mm=self.smoothing*(self.scale[parent] if parent is not None else scale),
                       endpoint_tangent_locked=tuple(map(int, chain)) in self.tangent_locked, **info)
            cuts = np.unique(np.r_[t, kt]); q = np.unique((cuts[:-1, None]+np.diff(cuts)[:, None]*np.linspace(0, 1, 5)).ravel())
            result.append(rec); samples.append(BSpline(kt, cp, 3)(q)); errors.append(error)
        return result, samples, errors

    def tighten(self, records):
        selected = set()
        for rec in records:
            ci = int(rec['parent_interface'])
            if ci >= 0:
                selected.add(ci)
            else:
                key = tuple(map(int, rec['chain']))
                if key in self.tangent_locked:
                    self.force_positive.add(key)
                    self.internal[key] = self.internal.get(key, 1.)*.5
                else:
                    self.tangent_locked.add(key)
                # Moving an endpoint of an internal cut requires moving its
                # parent interface as well; shrinking only the cut is infeasible.
                for vi in rec['chain'][[0, -1]]:
                    selected.update(self.vertex_parents.get(int(vi), ()))
        for ci in selected:
            self.scale[ci] *= .5
        self.refit(selected)

    def seed(self, curves):
        """Reuse a checked network only on exactly the same source chains."""
        if self.chains:
            return
        for rec in curves:
            if 'method' not in rec:
                continue
            chain = rec['chain']; key = tuple(map(int, chain))
            scale = float(rec.get('movement_budget_mm', self.smoothing))/self.smoothing
            locked = bool(rec.get('endpoint_tangent_locked', False))
            positive = str(rec['method']) == 'positive_topology_guard'
            if locked and not positive and 'movement_budget_mm' not in rec:
                # An older network may have shrunk its budget almost to zero
                # to repair a free-tangent loop. Try the positive smooth guide
                # at the declared budget before inheriting a rasterized trim.
                self.internal[key] = 1.
                self.tangent_locked.add(key); self.force_positive.add(key)
                continue
            self.internal[key] = scale
            if locked: self.tangent_locked.add(key)
            if positive: self.force_positive.add(key)
            fitted = (rec['poles'], rec['knots'], float(rec['movement_mm']),
                dict(method=str(rec['method']), source_plane_error_mm=float(rec['source_plane_error_mm'])))
            self.internal_cache[key] = scale, self.v[chain[[0, -1]]].copy(), locked, positive, fitted
