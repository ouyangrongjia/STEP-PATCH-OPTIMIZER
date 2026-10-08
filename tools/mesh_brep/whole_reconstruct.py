"""Native fixed-region STL reconstruction with a strict reference face budget.

Reference CAD is read only by the independent face-count evaluator. The fitter
receives STL, supplied labels and a scalar budget. No commercial reconstruction
module, executable, cached fit, CAD surface or CAD curve is used.
"""
import argparse
from dataclasses import asdict
from pathlib import Path
import time
import numpy as np
from .mesh import dump
from .partition_fit import load_partition
from .partition_network import simplify
from .regional_reconstruct import Config, quad_layout


def select_layout(v, f, labels, face_limit, errors, target_triangles, output):
    records=[];feasible=[]
    for error in errors:
        started=time.perf_counter()
        try:
            p,t,r,ids,reduction=simplify(v,f,labels,error,target_triangles,anchors='vertex')
            layout=quad_layout(p,t,r,matching='maximum')
            record=dict(error_mm=error,triangles=len(t),patches=len(layout),simplification=reduction,
                        face_budget_passed=len(layout)<=face_limit)
            if record['face_budget_passed']:feasible.append(record)
        except ValueError as exc:record=dict(error_mm=error,error=str(exc),face_budget_passed=False)
        record['seconds']=time.perf_counter()-started;records.append(record);dump(output,records)
    if not feasible:raise ValueError('No native layout meets the reference face budget; see layout_search.json')
    # Coarse layout tolerance is not a final geometric error claim. Prefer the
    # least simplified feasible layout, then retain more patches for fidelity.
    return min(feasible,key=lambda r:(r['error_mm'],-r['patches']))


def reconstruct(stl, partition, output, reference=None, reference_faces=None,
                layout_errors=None, degree=7, levels=3, iterations=5,
                seam_weight=.05, target_triangles=None, creo_root=None, render=True):
    out=Path(output);out.mkdir(parents=True,exist_ok=False);started=time.perf_counter()
    report=dict(status='running',can_apply=False,algorithm='native-fixed-regions-budgeted-shared-detail-v2',
                stl=str(Path(stl).resolve()),partition=str(Path(partition).resolve()),
                commercial_engine_used=False,fitter_reads_source_CAD=False,units='mm')
    dump(out/'run.json',report)
    try:
        if (reference is None)==(reference_faces is None):
            raise ValueError('Provide exactly one reference STEP or independently measured reference face count')
        if reference is not None:
            from .reference_catalog import transferred_face_count
            reference_faces=transferred_face_count(reference)
            report['reference']=str(Path(reference).resolve())
        if reference_faces<2:raise ValueError('Reference face count leaves no reconstruction budget')
        face_limit=int(reference_faces)-1
        report.update(reference_faces=int(reference_faces),maximum_reconstructed_faces=face_limit)
        v,f,labels,topology=load_partition(partition,stl)
        report['input_topology']=topology
        if layout_errors is None:
            scale=float(np.linalg.norm(np.ptp(v,axis=0)))
            layout_errors=[scale*x for x in (.0025,.0035,.005,.0075,.01,.015)]
        requested=int(target_triangles if target_triangles is not None else max(4,face_limit*.8))
        selected=select_layout(v,f,labels,face_limit,layout_errors,requested,out/'layout_search.json')
        config=Config(simplify_error=selected['error_mm'],target_triangles=requested,degree=degree,seam_weight=seam_weight)
        report.update(config=asdict(config),selected_layout=selected,levels=levels,iterations=iterations)
        dump(out/'run.json',report)
        from .subdivision_reconstruct import reconstruct as fit
        report['fit']=fit(partition,stl,out/'fit',config,levels=levels,iterations=iterations,mesh_features=True,
                          anchors='vertex',matching='maximum',detail_projection=True,face_limit=face_limit)
        from .surface_safeguard import safeguard
        report['assembly']=safeguard(out/'fit',out/'assembled')
        step=out/'assembled/model.step'
        actual=int(report['assembly']['topology']['faces'])
        report['face_budget_passed']=actual<int(reference_faces)
        if not report['face_budget_passed']:raise ValueError(f'Exported STEP exceeds budget: {actual} >= {reference_faces}')
        from .step_quality import audit
        report['quality']=audit(step,partition,out/'quality',render=render)
        if creo_root is not None:
            from .creo_validation import creo_check
            report['creo']=creo_check(step,out/'creo',creo_root)
        report.update(status='DiagnosticOnly',model=str(step.resolve()),
                      geometry_acceptance='Review independent distance, seam normals, intersections and actual Creo evidence')
    except Exception as exc:
        report.update(status='failed',error=f'{type(exc).__name__}: {exc}');raise
    finally:
        report['wall_seconds']=time.perf_counter()-started;dump(out/'run.json',report)
    return report


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--stl',required=True);p.add_argument('--partition',required=True)
    p.add_argument('--output',required=True);group=p.add_mutually_exclusive_group(required=True)
    group.add_argument('--reference');group.add_argument('--reference-faces',type=int)
    p.add_argument('--layout-errors',type=float,nargs='+');p.add_argument('--target-triangles',type=int)
    p.add_argument('--degree',type=int,choices=[5,7,9],default=7);p.add_argument('--levels',type=int,default=3)
    p.add_argument('--iterations',type=int,default=5);p.add_argument('--seam-weight',type=float,default=.05)
    p.add_argument('--creo-root');p.add_argument('--no-render',action='store_true');a=p.parse_args()
    reconstruct(a.stl,a.partition,a.output,a.reference,a.reference_faces,a.layout_errors,a.degree,a.levels,
                a.iterations,a.seam_weight,a.target_triangles,a.creo_root,not a.no_render)
