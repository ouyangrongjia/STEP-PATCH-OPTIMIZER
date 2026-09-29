import json, os, sys, time
from pathlib import Path
root=Path(__file__).resolve().parent
sys.path.insert(0,str(root/'code'))
from mesh_brep import reconstruct, cad
from OCP.STEPControl import STEPControl_Reader
from OCP.IFSelect import IFSelect_RetDone
manifest=json.loads((root/'lock_manifest.json').read_text(encoding='utf-8'))
probes=[]
for path in manifest['probes']:
    try:
        with open(path,'rb') as f: f.read(1)
    except OSError as error:
        probes.append(dict(path=path,python_denied=True,winerror=error.winerror))
    else: raise RuntimeError('OS lock did not deny '+path)
    if Path(path).suffix.lower() in ['.step','.stp']:
        status=STEPControl_Reader().ReadFile(path)
        probes[-1]['native_step_reader_denied']=status!=IFSelect_RetDone
        if status==IFSelect_RetDone: raise RuntimeError('Native CAD read unexpectedly allowed')
data_inputs={str((root/'input/input.stl').resolve()),str((root/'input/layout.json').resolve())}
runtime=[Path(sys.prefix).resolve(),Path(sys.base_prefix).resolve(),root/'code']
opened=set(); denied=[]
output=root/'result'
def access(event,args):
    if event!='open' or not isinstance(args[0],(str,bytes,os.PathLike)): return
    path=Path(os.fsdecode(args[0])).resolve()
    if str(path) in data_inputs: opened.add(str(path)); return
    if path.is_relative_to(output) or any(path.is_relative_to(r) for r in runtime): return
    denied.append(str(path)); raise PermissionError('Undeclared reconstruction file '+str(path))
sys.addaudithook(access)
original_reader=cad.read_generated
def guarded_reader(path):
    if not Path(path).resolve().is_relative_to(output): raise PermissionError('Only newly generated STEP may be reread')
    return original_reader(path)
cad.read_generated=guarded_reader
started=time.perf_counter()
report=reconstruct.reconstruct(root/'input/input.stl',root/'input/layout.json',output,manifest['controls'])
evidence=dict(probes=probes,opened_data_files=sorted(opened),denied_unexpected_reads=denied,locked_files=len(manifest['files']),
              reconstruction_seconds=time.perf_counter()-started,completed=True,
              limitation='No adversarial native-code sandbox claim; audited trusted backend plus actual OS sharing-denial of CAD/cache files.')
(output/'isolation_evidence.json').write_text(json.dumps(evidence,indent=2,ensure_ascii=False),encoding='utf-8')
