"""Export source-labelled open regions from the actual assembled native BRep."""
import json
from pathlib import Path
import time
import numpy as np
from OCP.BRep import BRep_Builder, BRep_Tool
from OCP.BRepTools import BRepTools
from OCP.IFSelect import IFSelect_RetDone
from OCP.STEPControl import STEPControl_Writer, STEPControl_AsIs
from OCP.StepGeom import StepGeom_SurfaceCurve, StepGeom_pscrCurve3d
from OCP.TopAbs import TopAbs_FACE
from OCP.TopoDS import TopoDS_Shape, TopoDS_Compound
from . import cad
from .mesh import dump
from .partition_fit import surface_poles


def export(assembly, output, regions=None):
    root = Path(assembly); out = Path(output); out.mkdir(parents=True, exist_ok=False)
    started = time.perf_counter(); meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    source = Path(meta['source_fit'])
    records = [dict(np.load(p)) for p in sorted((source/'charts').glob('*.npz'))]
    shape = TopoDS_Shape(); builder = BRep_Builder()
    native = root/meta.get('native_file', 'native.brep')
    if native.suffix == '.binbrep':
        from OCP.BinTools import BinTools
        if not BinTools.Read_s(shape, str(native)):
            raise ValueError('Cannot read binary native assembly')
    else:
        BRepTools.Read_s(shape, str(native), builder)
    faces = cad.shapes(shape, TopAbs_FACE)
    mapping = meta.get('cad_face_charts', list(range(len(faces))))
    if len(faces) != len(records) or sorted(mapping) != list(range(len(records))):
        raise ValueError('Missing one-to-one native face/chart identity')
    # Older runs predate the explicit map. Verify every proposed identity by
    # actual native support poles, never by nearest-region majority voting.
    if meta.get('boundary_smoothing_budget_mm', 0):
        from .joint_graph_boundary import extend_support
        for rec in records:
            extend_support(rec, meta['boundary_smoothing_budget_mm']*1.1)
    groups = {}
    for face, chart in zip(faces, mapping):
        rec = records[chart]; support = BRep_Tool.Surface_s(face)
        actual = np.array([[support.Pole(i, j).Coord() for j in range(1, support.NbVPoles()+1)]
                           for i in range(1, support.NbUPoles()+1)])
        np.testing.assert_allclose(actual, surface_poles(rec), rtol=0, atol=1e-10,
                                   err_msg='Native face/chart geometry mismatch')
        groups.setdefault(int(rec['region']), []).append(face)
    report = dict(status='running', source_assembly=str(root.resolve()), source_fit=str(source.resolve()),
        source_CAD_read=False, source_region_identity='Verified native support poles and chart labels',
        scope='Open fitted regional surfaces before STEP healing; the assembled whole STEP carries solid validation.',
        files=[], complete_partition_export=regions is None)
    dump(out/'run.json', report)
    try:
        for region, selected in sorted(groups.items()):
            if regions is not None and region not in regions:
                continue
            compound = TopoDS_Compound(); builder.MakeCompound(compound)
            for face in selected:
                builder.Add(compound, face)
            path = out/f'region_{region:05d}.step'; writer = STEPControl_Writer()
            writer.SetTolerance(meta['topology_before']['maximum_edge_tolerance_mm'])
            writer.Transfer(compound, STEPControl_AsIs); model = writer.Model()
            for i in range(1, model.NbEntities()+1):
                entity = model.Value(i)
                if isinstance(entity, StepGeom_SurfaceCurve):
                    entity.SetMasterRepresentation(StepGeom_pscrCurve3d)
            if writer.Write(str(path)) != IFSelect_RetDone:
                raise RuntimeError(f'Cannot write region {region}')
            reread_faces = len(cad.shapes(cad.read_generated(path), TopAbs_FACE))
            if reread_faces != len(selected):
                raise ValueError(f'Region {region} changed face count on STEP reread')
            report['files'].append(dict(region=region, file=path.name, faces=reread_faces, bytes=path.stat().st_size))
        report.update(status='DiagnosticOnly', exported_regions=len(report['files']),
                      exported_faces=sum(x['faces'] for x in report['files']))
    except Exception as error:
        report.update(status='failed', error=f'{type(error).__name__}: {error}'); raise
    finally:
        report['seconds'] = time.perf_counter()-started; dump(out/'run.json', report)
    return report


if __name__ == '__main__':
    import argparse
    p = argparse.ArgumentParser(); p.add_argument('--assembly', required=True); p.add_argument('--output', required=True)
    p.add_argument('--regions', type=int, nargs='+')
    a = p.parse_args(); export(a.assembly, a.output, a.regions)
