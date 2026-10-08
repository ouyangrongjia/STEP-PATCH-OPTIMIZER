"""Exact regular-support conversion: three quads per triangular polynomial.

Existing quads stay as one face; only their trim edges are bisected where
necessary. This avoids both collapsed surface sides and extrapolation outside
a triangular trimming domain in the receiving CAD kernel.
"""
import numpy as np

from .regional_reconstruct import bernstein, tensor, edge_uv
from .regional_step import affine_triangle


def split_curve(cp):
    left, right = [cp[0]], [cp[-1]]; work = cp.copy()
    while len(work) > 1:
        work = (work[:-1]+work[1:])/2
        left.append(work[0]); right.append(work[-1])
    return np.array(left), np.array(right[::-1])


def convert(vertices, patches, network, selected=None):
    positions = list(np.asarray(vertices)); converted = []; edges = {}; midpoints = {}
    selected={i for i,p in enumerate(patches) if len(p['vertices'])==3} if selected is None else set(selected)
    split_edges={key for i,p in enumerate(patches) if i in selected and len(p['vertices'])==3 for key in p['edges']}
    def register(a, b, cp, sharp=False):
        key = tuple(sorted((int(a), int(b))))
        value = cp if a == key[0] else cp[::-1]
        if key not in edges:
            edges[key] = dict(cp=value, line=value.copy(), scale=1., owners=[], sharp=sharp)
        return key
    for key, rec in network.items():
        cp = rec['line']+rec['scale']*(rec['cp']-rec['line'])
        if key not in split_edges:
            register(key[0],key[1],cp,rec['sharp']);continue
        left, right = split_curve(cp); mid = len(positions); positions.append(left[-1]); midpoints[key] = mid
        register(key[0],mid,left,rec['sharp']); register(mid,key[1],right,rec['sharp'])
    def uses(loop, uv_corners=None):
        result = []
        for side,(a,b) in enumerate(zip(loop,np.roll(loop,-1))):
            key=tuple(sorted((int(a),int(b)))); forward=int(a)==key[0]
            uv=edge_uv(4,side,np.array([0.,1.])) if uv_corners is None else np.array([uv_corners[side],uv_corners[(side+1)%len(loop)]])
            result.append(dict(edge=key,forward=forward,uv=uv if forward else uv[::-1]))
        return result
    max_error=0.
    for pi,p in enumerate(patches):
        if len(p['vertices'])==4 or pi not in selected:
            loop=[];uv=[]
            for side,(a,key) in enumerate(zip(p['vertices'],p['edges'])):
                t=np.array([0.,.5]) if key in midpoints else np.array([0.])
                loop.append(a)
                if key in midpoints:loop.append(midpoints[key])
                if len(p['vertices'])==3:
                    from .regional_step import cad_edge_uv
                    uv.extend(cad_edge_uv(3,side,t,True))
                else:uv.extend(edge_uv(4,side,t))
            converted.append(dict(**{k:v for k,v in p.items() if k!='edges'},edges=[tuple(sorted((a,b))) for a,b in zip(loop,np.roll(loop,-1))],trim_uses=uses(loop,uv),parent_patch=pi))
            continue
        n=len(p['poles'])-1; affine=affine_triangle(p['poles']); origin=affine[0,0].copy(); local=(affine-origin).reshape(-1,3)
        uv_vertex=np.array([[0.,0.],[1.,0.],[0.,1.]])
        uv_mid=(uv_vertex+np.roll(uv_vertex,-1,axis=0))/2;center_uv=np.array([1/3,1/3])
        center=len(positions);positions.append(tensor(n,center_uv[None])@local+origin);positions[-1]=positions[-1][0]
        grid=np.linspace(0,1,n+1);inverse=np.linalg.inv(bernstein(n,grid))
        q=np.stack(np.meshgrid(grid,grid,indexing='ij'),axis=-1).reshape(-1,2)
        for side,key in enumerate(p['edges']):
            samples=(1-grid[:,None])*uv_mid[side]+grid[:,None]*center_uv
            cp=inverse@(tensor(n,samples)@local)+origin
            register(midpoints[key],center,cp)
        for side,a in enumerate(p['vertices']):
            previous=(side-1)%3
            loop=[a,midpoints[p['edges'][side]],center,midpoints[p['edges'][previous]]]
            corners=np.array([uv_vertex[side],uv_mid[side],center_uv,uv_mid[previous]])
            u,v=q[:,0,None],q[:,1,None]
            params=(1-u)*(1-v)*corners[0]+u*(1-v)*corners[1]+u*v*corners[2]+(1-u)*v*corners[3]
            samples=(tensor(n,params)@local).reshape(n+1,n+1,3)
            cp=np.einsum('ij,jkc,lk->ilc',inverse,samples,inverse)+origin
            # Same geometry checked at independent, non-collocation points.
            check=np.array([[.17,.29],[.63,.82],[.91,.11]])
            u,v=check[:,0,None],check[:,1,None]
            params=(1-u)*(1-v)*corners[0]+u*(1-v)*corners[1]+u*v*corners[2]+(1-u)*v*corners[3]
            residual=np.linalg.norm(tensor(n,check)@(cp-origin).reshape(-1,3)-tensor(n,params)@local,axis=1)
            max_error=max(max_error,float(residual.max()))
            converted.append(dict(vertices=loop,region=p['region'],poles=cp,edges=[tuple(sorted((int(x),int(y)))) for x,y in zip(loop,np.roll(loop,-1))],trim_uses=uses(loop),parent_patch=pi))
    for fi,p in enumerate(converted):
        for use in p['trim_uses']:edges[use['edge']]['owners'].append(fi)
    if any(len(e['owners'])!=2 for e in edges.values()):raise ValueError('Quad conversion lost shared edge ownership')
    return np.array(positions),converted,edges,dict(input_faces=len(patches),output_faces=len(converted),converted_triangles=sorted(selected),maximum_geometry_change_mm=max_error)


def convert_run(source, output, selected=None):
    import json
    import time
    from pathlib import Path
    from .mesh import dump
    from .regional_step import assemble
    source, output = Path(source), Path(output)
    output.mkdir(parents=True, exist_ok=False); started=time.perf_counter()
    data=np.load(source/'surfaces.npz'); meta=json.loads((source/'run.json').read_text(encoding='utf-8'))
    layout=json.loads((source/'fitting.json').read_text(encoding='utf-8'))['layout']
    patches=[];network={};v=np.zeros((max(k for p in layout for k in p['vertices'])+1,3))
    target_path=source/'subdivision_target.npz'
    if not target_path.exists() and 'target_source' in meta:
        target_path=Path(meta['target_source'])/'subdivision_target.npz'
    sharp={tuple(x) for x in np.load(target_path)['sharp']} if target_path.exists() else set()
    for pi,(p,cp) in enumerate(zip(layout,data['poles'])):
        p={**p,'edges':[tuple(k) for k in p['edges']],'poles':cp}
        ends=[cp[0,0],cp[-1,0],cp[-1,-1],cp[0,-1]] if len(p['vertices'])==4 else [cp[0,0],cp[-1,0],cp[0,-1]]
        for k,point in zip(p['vertices'],ends):v[k]=point
        boundaries=[cp[:,0],cp[-1,:],cp[::-1,-1],cp[0,::-1]] if len(p['vertices'])==4 else [cp[:,0],cp[-1,:],cp[0,::-1]]
        for key,a,curve in zip(p['edges'],p['vertices'],boundaries):
            curve=curve if a==key[0] else curve[::-1]
            if key not in network:network[key]=dict(cp=curve,line=curve.copy(),scale=1.,owners=[],sharp=key in sharp)
            elif np.linalg.norm(network[key]['cp']-curve,axis=1).max()>1e-8:raise ValueError('Source supports have inconsistent shared boundaries')
            network[key]['owners'].append(pi)
        patches.append(p)
    v,patches,network,conversion=convert(v,patches,network,selected)
    np.savez_compressed(output/'surfaces.npz',poles=np.array([p['poles'] for p in patches]),regions=np.array([p['region'] for p in patches]),corners=np.array([len(p['vertices']) for p in patches]))
    serial=[]
    for p in patches:
        rec={k:value for k,value in p.items() if k not in ('poles','trim_uses')}
        rec['vertices']=[int(k) for k in rec['vertices']];rec['edges']=[[int(k) for k in e] for e in rec['edges']]
        rec['trim_uses']=[dict(edge=[int(k) for k in use['edge']],forward=bool(use['forward']),uv=use['uv'].tolist()) for use in p['trim_uses']]
        serial.append(rec)
    dump(output/'fitting.json',dict(layout=serial,conversion=conversion))
    report={**meta,'status':'running','algorithm':'exact-regular-quad-conversion','geometry_source':str(source.resolve()),'conversion':conversion}
    try:report.update(assemble(v,patches,network,output,meta['config']['tolerance']),status='DiagnosticOnly')
    except Exception as error:report.update(status='failed',error=str(error));raise
    finally:
        report.update(wall_seconds=time.perf_counter()-started,scope='Exact representation conversion; fitting cost is in geometry_source')
        dump(output/'run.json',report)
    return report


if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser();parser.add_argument('--source',required=True);parser.add_argument('--output',required=True);parser.add_argument('--selected')
    args=parser.parse_args()
    import json
    from pathlib import Path
    convert_run(args.source,args.output,json.loads(Path(args.selected).read_text(encoding='utf-8')) if args.selected else None)
