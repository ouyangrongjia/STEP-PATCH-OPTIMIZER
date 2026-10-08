"""Fit fixed mesh partitions through a shared subdivision target to STEP.

Subdivision is an intermediate fitting model, not the delivered surface. The
export contains independently checked polynomial B-spline faces and common
three-dimensional trim edges. All fitting geometry comes from the input mesh.
"""
import argparse
from dataclasses import asdict
import json
from pathlib import Path
import time

import numpy as np
from scipy.linalg import null_space
from scipy import sparse

from .mesh import Locator, dump, stats, vertex_normals
from .partition_fit import load_partition
from .partition_network import simplify
from .regional_reconstruct import (Config, bernstein, tensor, elevate, quad_layout,
                                   make_network, edge_uv, evaluate)
from .subdivision_fit import fit_control_mesh


def triangle_transform(n):
    keys = [(a, b, n-a-b) for a in range(n+1) for b in range(n-a+1)]
    transform = np.zeros(((n+1)**2, len(keys)))
    for j, (a, b, c) in enumerate(keys):
        coeff = np.zeros((n-c+1, 1)); coeff[b] = 1
        transform.reshape(n+1, n+1, -1)[:, c, j] = elevate(coeff, n).ravel()
    return keys, transform


def support(boundary, corners, degree):
    boundary = [elevate(c, degree) for c in boundary]
    n = degree
    if len(boundary) == 3:
        keys, transform = triangle_transform(n); cp = []; free = []
        for j, (a, b, c) in enumerate(keys):
            if c == 0:
                cp.append(boundary[0][b])
            elif a == 0:
                cp.append(boundary[1][c])
            elif b == 0:
                cp.append(boundary[2][a])
            else:
                cp.append((a*corners[0]+b*corners[1]+c*corners[2])/n); free.append(j)
        return (transform@np.array(cp)).reshape(n+1, n+1, 3), transform[:, free]
    bottom, right, top, left = boundary
    top, left = top[::-1], left[::-1]
    t = np.linspace(0, 1, n+1); u, w = t[:, None, None], t[None, :, None]
    bilinear = ((1-u)*(1-w)*corners[0]+u*(1-w)*corners[1]+u*w*corners[2]+(1-u)*w*corners[3])
    poles = (1-w)*bottom[:, None]+w*top[:, None]+(1-u)*left[None]+u*right[None]-bilinear
    free = [i*(n+1)+j for i in range(1, n) for j in range(1, n)]
    return poles, np.eye((n+1)**2)[:, free]


class Target:
    def __init__(self, points, faces, parents, face_uv, coarse, labels, sharp_pairs=None):
        self.points = points; self.faces = faces; self.parents = parents
        self.uv = face_uv; self.coarse = coarse; self.labels = labels
        fine_labels = labels[parents]
        wedges, inverse = np.unique(np.c_[faces.ravel(), np.repeat(fine_labels, 3)], axis=0, return_inverse=True)
        cross = np.cross(points[faces[:, 1]]-points[faces[:, 0]], points[faces[:, 2]]-points[faces[:, 0]])
        accumulated = np.zeros((len(wedges), 3))
        np.add.at(accumulated, inverse, np.repeat(cross, 3, axis=0))
        if sharp_pairs is not None:
            from .partition_fit import incidence
            edges, owners = incidence(faces)
            parent_pair = np.sort(parents[owners], axis=1)
            encoded = parent_pair[:, 0]*len(coarse)+parent_pair[:, 1]
            sharp_codes = [min(a,b)*len(coarse)+max(a,b) for a,b in sharp_pairs]
            smooth = ~np.isin(encoded, sharp_codes)
            # Merge normal accumulators only across uncreased regional seams.
            selected = smooth & (fine_labels[owners[:, 0]] != fine_labels[owners[:, 1]])
            keys = wedges[:, 0]*(int(labels.max())+1)+wedges[:, 1]
            vertex = edges[selected].ravel()
            ra = np.repeat(fine_labels[owners[selected, 0]], 2)
            rb = np.repeat(fine_labels[owners[selected, 1]], 2)
            wa = np.searchsorted(keys, vertex*(int(labels.max())+1)+ra)
            wb = np.searchsorted(keys, vertex*(int(labels.max())+1)+rb)
            graph = sparse.coo_matrix((np.ones(len(wa)*2), (np.r_[wa,wb], np.r_[wb,wa])), shape=(len(wedges),len(wedges))).tocsr()
            count, component = sparse.csgraph.connected_components(graph, directed=False)
            grouped = np.zeros((count, 3)); np.add.at(grouped, component, accumulated)
            accumulated = grouped[component]
        accumulated /= np.maximum(np.linalg.norm(accumulated, axis=1)[:, None], 1e-30)
        self.region_normals = {int(r): (wedges[wedges[:,1] == r,0], accumulated[wedges[:,1] == r]) for r in np.unique(labels)}
        boundaries = np.searchsorted(parents, np.arange(len(coarse)+1))
        self.by_parent = [np.arange(boundaries[i],boundaries[i+1]) for i in range(len(coarse))]

    def chart(self, patch):
        corners = np.array([[0., 0.], [1., 0.], [1., 1.], [0., 1.]]) if len(patch['vertices']) == 4 else np.array([[0., 0.], [1., 0.], [0., 1.]])
        vertex_uv = {k: q for k, q in zip(patch['vertices'], corners)}
        params = []; triangles = []
        for fi in patch['source']:
            selected = self.by_parent[fi]; q = self.uv[selected]
            bary = np.concatenate([1-q.sum(axis=2, keepdims=True), q], axis=2)
            tri_uv = np.array([vertex_uv[k] for k in self.coarse[fi]])
            params.extend(np.einsum('nij,jk->nik', bary, tri_uv).reshape(-1, 2))
            triangles.extend(self.faces[selected].ravel())
        params = np.asarray(params); ids = np.asarray(triangles)
        local_faces = np.arange(len(params)).reshape(-1, 3)
        lookup = Locator(np.c_[params, np.zeros(len(params))], local_faces)
        used, normals = self.region_normals[patch['region']]
        normal = normals[np.searchsorted(used, ids)]
        def query(uv):
            if len(patch['vertices']) == 3:
                uv = np.c_[uv[:, 0]*(1-uv[:, 1]), uv[:, 1]]
            _, _, selected, bary = lookup.closest(np.c_[uv, np.zeros(len(uv))])
            local = local_faces[selected]
            xyz = np.einsum('ij,ijk->ik', bary, self.points[ids[local]])
            nn = np.einsum('ij,ijk->ik', bary, normal[local])
            nn /= np.maximum(np.linalg.norm(nn, axis=1)[:, None], 1e-30)
            return xyz, nn
        def face_normals(uv):
            if len(patch['vertices'])==3:uv=np.c_[uv[:,0]*(1-uv[:,1]),uv[:,1]]
            _,_,selected,_=lookup.closest(np.c_[uv,np.zeros(len(uv))])
            p=self.points[ids[local_faces[selected]]]
            n=np.cross(p[:,1]-p[:,0],p[:,2]-p[:,0])
            return n/np.maximum(np.linalg.norm(n,axis=1)[:,None],1e-30)
        query.face_normals=face_normals
        return query


def curve_fit(points, normals, n=5, endpoint_planes=None):
    from scipy.optimize import lsq_linear
    t = np.linspace(0, 1, len(points)); a = bernstein(n, t)
    origin = points[0]; end = points[-1]-origin
    tangents=[]
    for side, direction in [(0,points[1]-points[0]),(1,points[-1]-points[-2])]:
        if endpoint_planes is False:
            tangents.append(direction/max(np.linalg.norm(direction),1e-30));continue
        planes=[normals[0 if side==0 else -1]] if endpoint_planes is None else [nn for nn,s in endpoint_planes if s==side]
        _,singular,vh=np.linalg.svd(np.array(planes),full_matrices=True)
        rank=int(np.sum(singular>max(singular.max(),1e-30)*1e-7));space=vh[rank:].T
        tangent=space@(space.T@direction)
        if np.linalg.norm(tangent)<1e-12*np.linalg.norm(end):
            tangent=direction-planes[0]*np.dot(direction,planes[0])
        tangent/=max(np.linalg.norm(tangent),1e-30);tangents.append(tangent)
    base=np.zeros((n+1,3));base[-1]=end;base[-2]=end
    count=2+3*(n-3);mapping=np.zeros(((n+1)*3,count))
    mapping[3:6,0]=tangents[0];mapping[(n-1)*3:n*3,1]=-tangents[1]
    for j in range(2,n-1):mapping[j*3:(j+1)*3,2+(j-2)*3:2+(j-1)*3]=np.eye(3)
    matrix=np.kron(a,np.eye(3))@mapping;target=(points-origin-a@base).ravel()
    coeff=np.linalg.lstsq(matrix,target,rcond=1e-12)[0]
    arc=float(np.linalg.norm(np.diff(points,axis=0),axis=1).sum())
    lower=np.full(count,-np.inf);upper=np.full(count,np.inf)
    lower[:2]=arc*1e-8;upper[:2]=arc*2
    if np.any(coeff[:2]<lower[:2]) or np.any(coeff[:2]>upper[:2]):
        solved=lsq_linear(matrix,target,bounds=(lower,upper),tol=1e-10,max_iter=100)
        if not solved.success:raise ValueError('Directed shared curve fit failed')
        coeff=solved.x
    cp=base+(mapping@coeff).reshape(n+1,3)+origin
    return cp,float(np.linalg.norm(a@cp-points,axis=1).max())


def network_from_target(layout, charts, target_vertices, source_network, degree=5,tangent_policy='normal_plane'):
    network = {}; uses = {}
    for pi, p in enumerate(layout):
        p['edges'] = []
        for side, (a, b) in enumerate(zip(p['vertices'], np.roll(p['vertices'], -1))):
            key = tuple(sorted((int(a), int(b)))); p['edges'].append(key)
            uses.setdefault(key, []).append((pi, side, int(a) == key[0]))
    errors = []
    for key, owners in uses.items():
        values = []; normals = []; t = np.linspace(0, 1, 33)
        for pi, side, forward in owners:
            xyz, nn = charts[pi](edge_uv(len(layout[pi]['vertices']), side, t if forward else 1-t))
            values.append(xyz); normals.append(nn)
        error = float(np.linalg.norm(values[0]-values[1], axis=1).max())
        if error > 1e-8:
            raise ValueError(f'Subdivision target is not shared: {key}, {error}')
        common = normals[0]+normals[1]; common /= np.maximum(np.linalg.norm(common, axis=1)[:, None], 1e-30)
        sharp = source_network[key]['sharp']
        planes = [(ns[0],0) for ns in normals]+[(ns[-1],1) for ns in normals] if sharp else None
        if tangent_policy=='target':planes=False
        elif tangent_policy!='normal_plane':raise ValueError('Unknown curve tangent policy')
        cp, residual = curve_fit(values[0], common, degree, planes)
        cp[0], cp[-1] = target_vertices[key[0]], target_vertices[key[1]]
        network[key] = dict(cp=cp, line=cp.copy(), owners=[x[0] for x in owners],
                            sharp=sharp, scale=1., degree=degree, normal_t=t,
                            normal_values={layout[x[0]]['region']: normals[i] if sharp else common for i, x in enumerate(owners)})
        errors.append(residual)
    return network, stats(errors)


def fit_target_patch(patch, network, query, positions, degree=5, seam_weight=.01,scalar=False,normal_weight=0.,seam_mode='vector'):
    boundary = [network[key]['cp'] if a == key[0] else network[key]['cp'][::-1]
                for key, a in zip(patch['edges'], patch['vertices'])]
    poles, transform = support(boundary, positions[patch['vertices']], degree)
    origin = poles[0, 0].copy(); base = (poles-origin).reshape(-1, 3)
    grid = np.linspace(0., 1., max(13, degree*3+1))
    uv = np.stack(np.meshgrid(grid, grid, indexing='ij'), axis=-1).reshape(-1, 2)
    xyz, normal = query(uv)
    a = tensor(degree, uv); matrix = [np.kron(a@transform, np.eye(3))/np.sqrt(len(uv))]
    rhs = [(xyz-origin-a@base).ravel()/np.sqrt(len(uv))]
    size = max(np.linalg.norm(np.ptp(xyz, axis=0)), 1e-12)
    if normal_weight:
        weight=np.sqrt(normal_weight/len(uv))
        for du,dv in [(1,0),(0,1)]:
            op=tensor(degree,uv,du,dv)
            matrix.append(np.einsum('ni,nj->nij',op@transform,normal).reshape(len(uv),-1)*weight)
            rhs.append(-np.sum((op@base)*normal,axis=1)*weight)
    for side, (key, vertex) in enumerate(zip(patch['edges'], patch['vertices'])):
        t = np.linspace(.001, .999, 21); canonical = t if vertex == key[0] else 1-t
        edge = network[key]; tangent = bernstein(len(edge['cp'])-1, canonical, 1)@edge['cp']
        tangent /= np.maximum(np.linalg.norm(tangent, axis=1)[:, None], 1e-30)
        ns = edge['normal_values'][patch['region']]
        nn = np.stack([np.interp(canonical, edge['normal_t'], ns[:, k]) for k in range(3)], axis=1)
        nn -= tangent*np.sum(nn*tangent, axis=1)[:, None]
        nn /= np.maximum(np.linalg.norm(nn, axis=1)[:, None], 1e-30)
        q = edge_uv(len(patch['vertices']), side, t)
        axis = 1 if side == 0 or (len(patch['vertices']) == 4 and side == 2) else 0
        eps = 1e-3
        lo, hi = q.copy(), q.copy()
        lo[:, axis] = np.maximum(0., q[:, axis]-eps)
        hi[:, axis] = np.minimum(1., q[:, axis]+eps)
        derivative = (query(hi)[0]-query(lo)[0])/(hi[:, axis]-lo[:, axis])[:, None]
        derivative -= nn*np.sum(derivative*nn, axis=1)[:, None]
        op = tensor(degree, q, int(axis == 0), int(axis == 1))
        scale = np.maximum(np.linalg.norm(derivative, axis=1), size*.01)
        weight = np.sqrt(seam_weight/(len(t)*len(patch['edges'])))*size/scale
        if seam_mode=='plane':
            matrix.append(np.einsum('ni,nj->nij',op@transform,nn).reshape(len(t),-1)*weight[:,None])
            rhs.append(-np.sum((op@base)*nn,axis=1)*weight)
            # A weak signed transverse-speed prior resolves the orientation
            # ambiguity without forcing both regions to share UV distortion.
            direction=derivative/scale[:,None]
            matrix.append(np.einsum('ni,nj->nij',op@transform,direction).reshape(len(t),-1)*weight[:,None]*.03)
            rhs.append(np.sum((derivative-op@base)*direction,axis=1)*weight*.03)
        elif seam_mode=='vector':
            matrix.append(np.kron((op@transform)*weight[:, None], np.eye(3)))
            rhs.append(((derivative-op@base)*weight[:, None]).ravel())
        else:raise ValueError('Unknown seam fitting mode')
    # A small regularizer stabilizes only underdetermined polynomial modes.
    matrix.append(np.eye(transform.shape[1]*3)*1e-5); rhs.append(np.zeros(transform.shape[1]*3))
    matrix=np.vstack(matrix);rhs=np.concatenate(rhs)
    if scalar:
        p=positions[patch['vertices']]
        n=np.cross(p[1]-p[0],p[-1]-p[0]);n/=np.linalg.norm(n)
        direction=np.kron(np.eye(transform.shape[1]),n[:,None])
        delta=(direction@np.linalg.lstsq(matrix@direction,rhs,rcond=1e-11)[0]).reshape(-1,3)
    else:delta=np.linalg.lstsq(matrix,rhs,rcond=1e-11)[0].reshape(-1,3)
    poles = (base+transform@delta+origin).reshape(poles.shape)
    xyz_fit, nn, jac = evaluate(poles, uv)
    good = jac > 1e-14
    if len(patch['vertices']) == 3:
        good &= uv[:,1] < 1-1e-8
    dots = np.sum(nn*normal, axis=1)
    return poles, dict(target_distance_mm=stats(np.linalg.norm(xyz_fit-xyz, axis=1)),
                       minimum_target_normal_dot=float(dots[good].min()), opposed_samples=int(np.sum(dots[good] < 0)))


def reconstruct(partition, stl, output, config, levels=3, iterations=5, improve_mesh=False, triangles_only=False,mesh_features=False,
                anchors='triangle', matching='greedy', detail_projection=False, face_limit=None):
    out = Path(output); out.mkdir(parents=True, exist_ok=False); started = time.perf_counter()
    report = dict(status='running', algorithm='fixed-regions-loop-limit-bezier-v7-local-collision-guard', config=asdict(config),
                  levels=levels, iterations=iterations, stl=str(Path(stl).resolve()),
                  partition=str(Path(partition).resolve()), units='mm', source_CAD_read=False,
                  commercial_engine_used=False, anchors=anchors, matching=matching, detail_projection=detail_projection,
                  face_limit=face_limit)
    dump(out/'run.json', report)
    try:
        sv, sf, sl, topo = load_partition(partition, stl)
        v, f, labels, source_ids, reduction = simplify(sv, sf, sl, config.simplify_error, config.target_triangles,anchors=anchors)
        if improve_mesh:
            from .triangulation_quality import improve
            f,report['triangulation_improvement']=improve(v,f,labels,sv,sf,config.simplify_error)
        np.savez_compressed(out/'simplified_mesh.npz',vertices=v,triangles=f,labels=labels,source_ids=source_ids)
        report.update(input_topology=topo, simplification=reduction)
        nodal = {}
        for label in np.unique(sl):
            selected = sf[sl == label]; ids = np.unique(selected)
            normal = vertex_normals(sv[ids], np.searchsorted(ids, selected))
            nodal[int(label)] = {i: normal[np.searchsorted(ids, source_ids[i])] for i in np.unique(f[labels == label])}
        original = [dict(vertices=tri.tolist(), region=int(label)) for tri, label in zip(f, labels)]
        source_network = make_network(v, original, nodal, config.feature_angle)
        if mesh_features:
            from .partition_fit import incidence
            edge,owner=incidence(sf)
            normal=np.cross(sv[sf[:,1]]-sv[sf[:,0]],sv[sf[:,2]]-sv[sf[:,0]])
            normal/=np.maximum(np.linalg.norm(normal,axis=1)[:,None],1e-30)
            ridge=np.sum(normal[owner[:,0]]*normal[owner[:,1]],axis=1)<np.cos(np.radians(config.feature_angle))
            ridge_vertex=np.zeros(len(sv),dtype=bool);ridge_vertex[np.unique(edge[ridge])]=True
            before=int(sum(rec['sharp'] for rec in source_network.values()))
            for key,rec in source_network.items():rec['sharp']=bool(rec['sharp'] and ridge_vertex[source_ids[list(key)]].all())
            report['mesh_feature_filter']=dict(before=before,after=sum(rec['sharp'] for rec in source_network.values()),source_sharp_edges=int(ridge.sum()))
        sharp = {k for k, edge in source_network.items() if edge['sharp']}
        control, fine, faces, parents, face_uv, history, effective_sharp, safeguards = fit_control_mesh(v, f, sharp, sv, sf, levels, iterations)
        if detail_projection:
            np.savez_compressed(out/'base_target.npz',control=control,points=fine,triangles=faces,parents=parents,
                                face_uv=face_uv,sharp=np.array(list(effective_sharp)))
            from .mesh_detail_target import project_detail
            fine,report['detail_projection_report']=project_detail(fine,faces,parents,f,labels,sv,sf,sl,source_ids,out/'detail_backtracking.json')
        np.savez_compressed(out/'subdivision_target.npz', control=control, points=fine, triangles=faces,
                            parents=parents, face_uv=face_uv, coarse_triangles=f, coarse_regions=labels,
                            sharp=np.array(list(effective_sharp)),input_control=v,source_ids=source_ids)
        report.update(subdivision_fit=history, subdivision_collision_safeguards=safeguards, subdivision_seconds=time.perf_counter()-started,
                      original_crease_edges=len(sharp),effective_crease_edges=len(effective_sharp))
        for key in effective_sharp:source_network[key]['sharp']=True
        sharp_pairs = [tuple(source_network[key]['owners']) for key in effective_sharp]
        target = Target(fine, faces, parents, face_uv, f, labels, sharp_pairs)
        layout = ([dict(vertices=tri.tolist(),region=int(r),source=[i]) for i,(tri,r) in enumerate(zip(f,labels))]
                  if triangles_only else quad_layout(v, f, labels, effective_sharp if matching=='greedy' else sharp,matching=matching))
        if face_limit is not None and len(layout)>face_limit:
            raise ValueError(f'Native layout exceeds the reference face budget: {len(layout)} > {face_limit}')
        charts = [target.chart(p) for p in layout]
        positions = fine[:len(v)]
        network, boundary_error = network_from_target(layout, charts, positions, source_network, config.degree)
        report['boundary_target_distance_mm'] = boundary_error
        patches = []; records = []
        for i, (patch, chart) in enumerate(zip(layout, charts)):
            poles, rec = fit_target_patch(patch, network, chart, positions, config.degree, config.seam_weight)
            patches.append(dict(**patch, poles=poles)); records.append(dict(region=patch['region'], **rec))
            if i % 500 == 0:
                print(json.dumps(dict(stage='bezier', patch=i, total=len(layout), seconds=time.perf_counter()-started)), flush=True)
        np.savez_compressed(out/'surfaces.npz', poles=np.array([p['poles'] for p in patches]),
                            regions=np.array([p['region'] for p in patches]), corners=np.array([len(p['vertices']) for p in patches]))
        dump(out/'fitting.json', dict(patches=records, layout=layout, boundary_fit=boundary_error,
                                    opposed_samples=sum(r['opposed_samples'] for r in records)))
        report['fit_seconds'] = time.perf_counter()-started
        from .regional_step import assemble
        report.update(assemble(positions, patches, network, out, config.tolerance))
        report.update(status='DiagnosticOnly', can_apply=False)
    except Exception as error:
        report.update(status='failed', error=f'{type(error).__name__}: {error}'); raise
    finally:
        report['wall_seconds'] = time.perf_counter()-started; dump(out/'run.json', report)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(); parser.add_argument('--partition', required=True)
    parser.add_argument('--stl', required=True); parser.add_argument('--output', required=True)
    parser.add_argument('--simplify-error', type=float, default=.1)
    parser.add_argument('--degree', type=int, choices=[5, 7, 9], default=5)
    parser.add_argument('--seam-weight', type=float, default=.01)
    parser.add_argument('--levels', type=int, default=3); parser.add_argument('--iterations', type=int, default=5)
    parser.add_argument('--improve-mesh',action='store_true');parser.add_argument('--triangles-only',action='store_true')
    parser.add_argument('--mesh-features',action='store_true')
    args = parser.parse_args()
    reconstruct(args.partition, args.stl, args.output,
                Config(simplify_error=args.simplify_error, degree=args.degree, seam_weight=args.seam_weight),
                args.levels, args.iterations,args.improve_mesh,args.triangles_only,args.mesh_features)
