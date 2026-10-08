"""Frozen commercial experiment; not the current reconstruction entry.

The optional commercial fitting engine is Geomagic Wrap. All input geometry is
STL; region labels are supplied explicitly. OCCT sews and reconciles the fitted
network independently. This command never changes the original partition.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
import xml.etree.ElementTree as ET

import numpy as np
from .mesh import dump
from .partition_fit import incidence, load_partition


def prepare(stl, partition, output, distance=.03, patches=0):
    out = Path(output); out.mkdir(parents=True, exist_ok=False)
    v, f, labels, topo = load_partition(partition, stl)
    if topo['components'] != 1: raise ValueError('This backend requires one connected closed mesh')
    edges, owners = incidence(f); boundaries = edges[labels[owners[:,0]] != labels[owners[:,1]]]
    degree = np.bincount(boundaries.ravel(), minlength=len(v))
    junctions = np.flatnonzero((degree > 0) & (degree != 2))
    config = dict(stl=str(Path(stl).resolve()), partition=str(Path(partition).resolve()),
                  triangles=len(f), regions=len(np.unique(labels)), boundary_edges=len(boundaries),
                  junctions=len(junctions), units='mm', distance_mm=distance, num_patches=patches,
                  geometry='Organic', sharp_boundaries=False, export_native_step=False,
                  bounds=[v.min(axis=0).tolist(),v.max(axis=0).tolist()], topology=topo)
    dump(out/'input.json',config)
    dump(out/'constraints.json',dict(edges=v[boundaries].reshape(-1,6).tolist(),junctions=v[junctions].tolist()))
    shutil.copy2(stl,out/'input.stl')
    np.savez_compressed(out/'partition_snapshot.npz',mesh_vertex_original=v,mesh_triangle=f,face_instance_id=labels)
    return config


def run_wrap(directory, wrap_root, timeout=1800):
    root = Path(directory).resolve(); exe = Path(wrap_root)/'wrapCore.exe'
    if not exe.is_file(): raise FileNotFoundError(f'Wrap executable missing: {exe}')
    script = Path(__file__).with_name('wrap_fixed_regions.py').resolve()
    shutil.copy2(script,root/'fitting_script.py')
    env = os.environ.copy(); env['REGIONAL_WRAP_OUTPUT'] = str(root)
    with (root/'stdout.log').open('w',encoding='utf-8') as stdout, (root/'stderr.log').open('w',encoding='utf-8') as stderr:
        process = subprocess.Popen([str(exe),'--script',str(root/'fitting_script.py')],cwd=root,
                                   env=env,stdout=stdout,stderr=stderr,
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
        dump(root/'process.json',dict(pid=process.pid,timeout_seconds=timeout))
        try: code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.terminate(); process.wait(timeout=30)
            raise RuntimeError('Declared Wrap fitting time budget exhausted')
    record = json.loads((root/'run.json').read_text(encoding='utf-8'))
    # wrapCore can return 1 after a completed script with empty stderr. Preserve
    # that process fact; an explicit script completion and readable IGES are
    # required, and independent CAD validation still follows.
    dump(root/'process.json',dict(pid=process.pid,timeout_seconds=timeout,exit_code=code))
    if record.get('stage') != 'complete' or not (root/'surfaces.igs').is_file():
        raise RuntimeError(f'Wrap fit failed: {record.get("error",code)}')
    record['process_exit_code'] = code
    return record


def reuse_fit(source, destination):
    """Reuse an explicitly selected completed fit, with exact input checks."""
    source, destination = Path(source), Path(destination)
    if (source/'input.stl').read_bytes() != (destination/'input.stl').read_bytes():
        raise ValueError('Cached STL snapshot differs')
    old = json.loads((source/'input.json').read_text(encoding='utf-8'))
    new = json.loads((destination/'input.json').read_text(encoding='utf-8'))
    # These are the documented defaults used by wrap_fixed_regions itself.
    for key,value in dict(num_patches=0,geometry='Organic',sharp_boundaries=False).items():
        old.setdefault(key,value)
    keys = ['units', 'distance_mm', 'num_patches', 'geometry', 'sharp_boundaries']
    if any(old[k] != new[k] for k in keys): raise ValueError('Cached fitting settings differ')
    if old.get('engine_merge',False) != new.get('engine_merge',False): raise ValueError('Cached engine merging setting differs')
    if old.get('adaptive_fit',True) != new.get('adaptive_fit',True): raise ValueError('Cached adaptive fitting setting differs')
    for name in ['edges', 'junctions']:
        a = json.loads((source/'constraints.json').read_text(encoding='utf-8'))[name]
        b = json.loads((destination/'constraints.json').read_text(encoding='utf-8'))[name]
        if a != b: raise ValueError('Cached boundary network differs')
    old_labels=source/'partition_snapshot.npz';new_labels=destination/'partition_snapshot.npz'
    with np.load(old_labels if old_labels.exists() else old['partition']) as a, np.load(new_labels if new_labels.exists() else new['partition']) as b:
        for name in ['mesh_vertex_original', 'mesh_triangle', 'face_instance_id']:
            if not np.array_equal(a[name], b[name]): raise ValueError(f'Cached partition differs: {name}')
    record = json.loads((source/'run.json').read_text(encoding='utf-8'))
    if record.get('stage') != 'complete': raise ValueError('Cached fitting did not complete')
    shutil.copy2(source/'surfaces.igs', destination/'surfaces.igs')
    dump(destination/'run.json', record)
    return dict(reused_from=str(source.resolve()), original_run=record)


def creo_check(step, output, creo_root, timeout=300):
    """Use a task-local import profile and read actual ModelCHECK facts."""
    root = Path(output).resolve(); root.mkdir(parents=True, exist_ok=False)
    template = Path(creo_root)/'Common Files/x86e_win64/mworks/mw_import_mm.dip'
    profile = template.read_text(encoding='utf-8')
    settings = dict(ModelAccuracy='External', UseImptTemplate='1', KeepBSplineSrf='1',
                    SimplifySurface='0', ArclenCrvReparam='0', SurfBndryPref='UV', EnableATB='0')
    for key, value in settings.items():
        profile, count = re.subn(r'\b'+key+r'="[^"]*"', key+'="'+value+'"', profile)
        if count != 1: raise ValueError(f'Unexpected installed Creo profile attribute: {key}')
    (root/'import_mm.dip').write_text(profile, encoding='utf-8')
    runner = Path(__file__).with_name('creo_import_profile.ps1')
    with (root/'runner.log').open('w', encoding='utf-8') as log:
        proc = subprocess.run([shutil.which('pwsh') or 'powershell.exe', '-NoProfile', '-File', str(runner), '-Step', str(Path(step).resolve()),
                               '-OutputDir', str(root/'audit'), '-CreoRoot', str(creo_root),
                               '-Profile', str(root/'import_mm.dip'), '-TimeoutSeconds', str(timeout)],
                              stdout=log, stderr=subprocess.STDOUT, timeout=2*timeout+60,
                              creationflags=subprocess.CREATE_NO_WINDOW)
    if proc.returncode: raise RuntimeError('Creo diagnostic failed; see runner.log')
    reports = list((root/'audit/modelcheck').glob('*.p.xml'))
    if len(reports) != 1: raise ValueError('Missing unambiguous Creo ModelCHECK report')
    checks = {c.attrib['name']: c for c in ET.parse(reports[0]).findall('.//check')}
    params = {i.findtext('info1'): i.findtext('info2') for i in checks['PARAM_INFO'].findall('item')}
    result = dict(part_status=params.get('PTC_VAL_IMP_PART_STATUS'), score=params.get('PTC_VAL_IMP_SCORE'),
                  geometry_check_features=int(checks['GEOM_CHECKS'].findtext('ans')),
                  profile=str(root/'import_mm.dip'), report=str(reports[0]), gray_display_observed=False)
    result['solid_import_passed'] = result['part_status'] == 'OK' and result['score'] == 'PASS'
    dump(root/'run.json', result); return result


def sew_iges(source, output, tolerance=1e-6):
    from OCP.IGESControl import IGESControl_Reader
    from OCP.IFSelect import IFSelect_RetDone
    from OCP.BRepBuilderAPI import BRepBuilderAPI_Sewing, BRepBuilderAPI_MakeSolid
    from OCP.BRepTools import BRepTools
    from OCP.BRepLib import BRepLib
    from OCP.TopAbs import TopAbs_FACE, TopAbs_SHELL
    from . import cad
    from .partition_cad import topology
    out = Path(output); out.mkdir(parents=True,exist_ok=False); started = time.perf_counter()
    reader = IGESControl_Reader()
    if reader.ReadFile(str(source)) != IFSelect_RetDone: raise ValueError('Generated IGES could not be read')
    reader.TransferRoots(); shape = reader.OneShape()
    sew = BRepBuilderAPI_Sewing(tolerance,True,True,True,False)
    sew.SetMinTolerance(tolerance); sew.SetMaxTolerance(tolerance)
    for face in cad.shapes(shape,TopAbs_FACE): sew.Add(face)
    sew.Perform(); result = sew.SewedShape(); report = topology(result)
    if report['free_edges'] == 0 and report['shells'] == 1:
        result = BRepBuilderAPI_MakeSolid(cad.shapes(result,TopAbs_SHELL)[0]).Solid(); BRepLib.OrientClosedSolid_s(result)
        report = topology(result)
    BRepTools.Write_s(result,str(out/'model.brep')); cad.write_step(out/'model.step',result)
    report.update(seconds=time.perf_counter()-started,tolerance_mm=tolerance)
    dump(out/'run.json',report); return report


def reconstruct(stl, partition, output, wrap_root, distance=.03, patches=0, timeout=1800,
                max_seam_control_gap=.1, camera=None, render=True, merge_rounds=0,
                creo_root=None, cached_fit=None):
    root = Path(output).resolve(); root.mkdir(parents=True,exist_ok=False); started = time.perf_counter()
    report = dict(status='running',backend='Geomagic Wrap fixed-region fit + OCCT shared spline seams',
                  stl=str(Path(stl).resolve()),partition=str(Path(partition).resolve()),units='mm',source_CAD_read=False,
                  settings=dict(distance_mm=distance,patches=patches,merge_rounds=merge_rounds,
                                max_seam_control_gap_mm=max_seam_control_gap,timeout_seconds=timeout))
    dump(root/'run.json',report)
    try:
        report['input'] = prepare(stl,partition,root/'fit',distance,patches)
        partition=root/'fit/partition_snapshot.npz'
        report['fitting'] = reuse_fit(cached_fit, root/'fit') if cached_fit else run_wrap(root/'fit',wrap_root,timeout)
        report['sewing'] = sew_iges(root/'fit/surfaces.igs',root/'sewn')
        candidate = root/'sewn'
        if report['sewing']['free_edges']:
            from .repair_spline_seams import reconcile
            report['seam_reconciliation'] = reconcile(candidate/'model.brep',root/'shared',max_seam_control_gap)
            candidate = root/'shared'
        if merge_rounds:
            from .merge_spline_faces import merge
            report['merging'] = merge(candidate/'model.brep', partition, root/'merged', rounds=merge_rounds)
            candidate = root/'merged'
        from .step_quality import audit
        report['quality'] = audit(candidate/'model.step',partition,root/'quality',render=render,camera=camera)
        topo = report['quality']['topology']
        from .region_correspondence import audit as check_regions
        report['region_correspondence'] = check_regions(root/'quality',root/'region_correspondence.json')
        report['watertight_valid_solid'] = bool(topo['solids']==1 and topo['brep_valid'] and topo['free_edges']==0 and topo['multiple_edges']==0)
        report['model'] = str(candidate/'model.step')
        report['external_CAD'] = 'not_run'
        report['status'] = 'ValidSolidAwaitingExternalCAD' if report['watertight_valid_solid'] else 'DiagnosticOnly'
        if report['watertight_valid_solid']:
            shutil.copy2(candidate/'model.step', root/'model.step')
            report['model'] = str(root/'model.step')
            if creo_root:
                report['external_CAD'] = creo_check(root/'model.step', root/'creo', creo_root)
                external = report['external_CAD']
                if external['solid_import_passed']:
                    report['status'] = 'CreoSolidWithGeometryWarnings' if external['geometry_check_features'] else 'CreoSolidVerified'
                else: report['status'] = 'CreoImportFailed'
        report['can_apply'] = False
    except Exception as error:
        report.update(status='failed',error=f'{type(error).__name__}: {error}'); raise
    finally:
        report['wall_seconds'] = time.perf_counter()-started; dump(root/'run.json',report)
    return report


if __name__ == '__main__':
    p=argparse.ArgumentParser();p.add_argument('--stl',required=True);p.add_argument('--partition',required=True)
    p.add_argument('--output',required=True);p.add_argument('--wrap-root',required=True)
    p.add_argument('--units',choices=['mm'],required=True,help='Declare the coordinate unit; no implicit rescaling')
    p.add_argument('--distance-mm',type=float,default=.03);p.add_argument('--patches',type=int,default=0)
    p.add_argument('--timeout',type=float,default=1800);p.add_argument('--max-seam-control-gap',type=float,default=.1)
    p.add_argument('--camera');p.add_argument('--no-render',action='store_true')
    p.add_argument('--merge-rounds',type=int,default=0,help='Optional exact within-region merging; default 0 preserves CAD compatibility')
    p.add_argument('--creo-root',help='Optional installed Creo root for actual external solid checks')
    p.add_argument('--reuse-fit',help='Explicit completed fit directory; exact inputs/settings must match')
    a=p.parse_args()
    if a.distance_mm<=0 or a.patches<0 or a.max_seam_control_gap<=0 or a.merge_rounds<0 or a.timeout<=0: p.error('Invalid numerical budget')
    result=reconstruct(a.stl,a.partition,a.output,a.wrap_root,a.distance_mm,a.patches,a.timeout,
                       a.max_seam_control_gap,a.camera,not a.no_render,a.merge_rounds,a.creo_root,a.reuse_fit)
    if not result['watertight_valid_solid'] or result['status']=='CreoImportFailed':raise SystemExit(2)
