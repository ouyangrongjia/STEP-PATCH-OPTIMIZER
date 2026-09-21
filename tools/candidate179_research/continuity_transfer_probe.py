"""Separate support defects from STEP pcurve/trim transfer on the same geometry."""
import argparse
from pathlib import Path
from geometry import *
from OCP.Interface import Interface_Static
from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeFace


def write_without_pcurves(path,shape):
    # Instantiate a writer first so its parameters exist, then restore the
    # process-wide setting. This is an export comparison, not a CAD tolerance.
    STEPControl_Writer();old=Interface_Static.IVal_s('write.surfacecurve.mode')
    try:
        if not Interface_Static.SetIVal_s('write.surfacecurve.mode',0):raise RuntimeError('STEP writer option unavailable')
        write_step(path,shape)
    finally:Interface_Static.SetIVal_s('write.surfacecurve.mode',old)


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);parser.add_argument('--candidate',default='aligned_inner_cubic');args=parser.parse_args()
    out=Path(args.run);target=out/args.candidate/'transfer_probe';target.mkdir(exist_ok=True)
    shape=read_step(out/args.candidate/'patch_faces.step');face=shapes(shape,TopAbs_FACE)[1]
    support=BRepBuilderAPI_MakeFace(BRep_Tool.Surface_s(face),1e-7).Face()
    if face.Orientation()==TopAbs_REVERSED:support=TopoDS.Face_s(support.Reversed())
    write_step(target/'face_1_support.step',support)
    write_without_pcurves(target/'face_1_3donly.step',face)
    dump(target/'protocol.json',dict(source=str(out/args.candidate/'patch_faces.step'),face=1,
        source_topology=topology(face),support_topology=topology(support),writer_setting={'write.surfacecurve.mode':0},
        cad_template_changed=False,engineering_acceptance='Unvalidated diagnostic probes; no acceptance implied by an import result.'))


if __name__=='__main__':main()
