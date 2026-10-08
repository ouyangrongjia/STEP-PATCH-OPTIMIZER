"""Export native graph BReps with canonical 3D boundary representation."""
import argparse
import json
from pathlib import Path
import time
from OCP.BRep import BRep_Builder
from OCP.BRepTools import BRepTools
from OCP.TopoDS import TopoDS_Shape
from OCP.STEPControl import STEPControl_Writer, STEPControl_AsIs
from OCP.StepGeom import StepGeom_SurfaceCurve, StepGeom_pscrCurve3d
from OCP.IFSelect import IFSelect_RetDone
from OCP.Interface import Interface_Static
from .partition_cad import topology
from .mesh import dump
from . import cad


def export(run, output, write_pcurves=True):
    root = Path(run); out = Path(output); out.mkdir(parents=True, exist_ok=False)
    started = time.perf_counter(); source = json.loads((root/'run.json').read_text(encoding='utf-8'))
    report = dict(status='running', can_apply=False, commercial_engine_used=False,
        source_fit=str(root.resolve()), partition=source['partition'], stl=source['stl'],
        algorithm='native-graph-step-canonical-3d-boundaries-v1', write_pcurves=bool(write_pcurves))
    dump(out/'run.json', report)
    try:
        shape = TopoDS_Shape(); BRepTools.Read_s(shape, str(root/'native.brep'), BRep_Builder())
        before = topology(shape)
        expected_solids = source.get('expected_solids', 1)
        if not before['brep_valid'] or before['free_edges'] or before['solids'] != expected_solids:
            raise ValueError('Source native BRep must already contain the expected valid closed bodies')
        writer = STEPControl_Writer(); writer.SetTolerance(before['maximum_edge_tolerance_mm'])
        previous = Interface_Static.IVal_s('write.surfacecurve.mode')
        try:
            if not Interface_Static.SetIVal_s('write.surfacecurve.mode', int(write_pcurves)):
                raise RuntimeError('Cannot set STEP pcurve export mode')
            writer.Transfer(shape, STEPControl_AsIs)
        finally:
            Interface_Static.SetIVal_s('write.surfacecurve.mode', previous)
        model = writer.Model(); changed = 0
        for index in range(1, model.NbEntities()+1):
            entity = model.Value(index)
            if isinstance(entity, StepGeom_SurfaceCurve):
                entity.SetMasterRepresentation(StepGeom_pscrCurve3d); changed += 1
        if writer.Write(str(out/'model.step')) != IFSelect_RetDone:
            raise RuntimeError('STEP write failed')
        after = topology(cad.read_generated(out/'model.step'))
        report.update(status='DiagnosticOnly', topology_before=before, topology=after,
            master_3d_curves=changed, declared_step_uncertainty_mm=before['maximum_edge_tolerance_mm'],
            step_bytes=(out/'model.step').stat().st_size,
            expected_solids=expected_solids,
            watertight=bool(after['brep_valid'] and after['solids'] == expected_solids and after['free_edges'] == 0
                and after['multiple_edges'] == 0 and
                after['maximum_edge_tolerance_mm'] <= source['boundary_tolerance_mm']))
    except Exception as error:
        report.update(status='failed', error=f'{type(error).__name__}: {error}'); raise
    finally:
        report['seconds'] = time.perf_counter()-started; dump(out/'run.json', report)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(); parser.add_argument('--run', required=True); parser.add_argument('--output', required=True)
    parser.add_argument('--without-pcurves', action='store_true')
    args = parser.parse_args(); export(args.run, args.output, not args.without_pcurves)
