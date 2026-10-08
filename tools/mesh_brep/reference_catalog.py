"""Independent reference face-count inventory, never a fitting input reader."""
import argparse
from pathlib import Path
import re
from .mesh import dump


def transferred_face_count(path):
    """Evaluation only: count unique transferred faces, not STEP text entities."""
    from OCP.STEPControl import STEPControl_Reader
    from OCP.IFSelect import IFSelect_RetDone
    from OCP.TopExp import TopExp
    from OCP.TopAbs import TopAbs_FACE
    from OCP.TopTools import TopTools_IndexedMapOfShape
    reader=STEPControl_Reader()
    if reader.ReadFile(str(path))!=IFSelect_RetDone or reader.TransferRoots()<1:
        raise ValueError(f'Cannot transfer reference STEP: {path}')
    faces=TopTools_IndexedMapOfShape();TopExp.MapShapes_s(reader.OneShape(),TopAbs_FACE,faces)
    if faces.Extent()<1:raise ValueError('Reference has no transferred faces')
    return faces.Extent()


def inventory(directory, output, stl_directory=None):
    root=Path(directory);records=[]
    for path in sorted(root.iterdir()):
        if path.suffix.lower() not in {'.stp','.step'}:continue
        text=path.read_text(encoding='ascii',errors='replace')
        advanced=len(re.findall(r'=\s*ADVANCED_FACE\s*\(',text))
        basic=len(re.findall(r'=\s*FACE_SURFACE\s*\(',text))
        record=dict(reference=str(path.resolve()),face_entities=advanced+basic,
                    count_scope='STEP face entity inventory; final gate additionally reads transferred topology',
                    maximum_reconstructed_faces=advanced+basic-1)
        if stl_directory:
            stl=Path(stl_directory)/(path.stem+'.stl');record['stl']=str(stl.resolve()) if stl.is_file() else None
        records.append(record)
    dump(output,dict(references=records,count=len(records),reconstruction_reads_reference_geometry=False))
    return records


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--directory',required=True);p.add_argument('--output',required=True)
    p.add_argument('--stl-directory');a=p.parse_args()
    for r in inventory(a.directory,a.output,a.stl_directory):print(r['reference'],r['face_entities'])
