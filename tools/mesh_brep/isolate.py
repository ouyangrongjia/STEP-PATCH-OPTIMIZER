"""Windows replay with OS-denied CAD/cache reads and a minimal input package.

The parent holds exclusive read handles (share mode 0) on forbidden files.
No ACL, source file or original directory is modified. The child also audits
Python file access. Native OCCT denial is tested before reconstruction.
This is a reproducibility barrier, not a sandbox against hostile native code.
"""
import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time


def main():
    p = argparse.ArgumentParser(); p.add_argument('--stl', required=True); p.add_argument('--layout', required=True)
    p.add_argument('--output', required=True); p.add_argument('--deny-root', action='append', required=True)
    p.add_argument('--probe', action='append', required=True); p.add_argument('--controls', type=int, default=29)
    a = p.parse_args()
    if os.name != 'nt':
        raise RuntimeError('Exclusive-handle replay is implemented for Windows')
    out = Path(a.output).resolve(); out.mkdir(parents=True, exist_ok=False)
    code = out/'code'/'mesh_brep'; code.mkdir(parents=True)
    for name in ['__init__.py', 'mesh.py', 'splines.py', 'cad.py', 'reconstruct.py']:
        shutil.copy2(Path(__file__).parent/name, code/name)
    inputs = out/'input'; inputs.mkdir()
    shutil.copy2(a.stl, inputs/'input.stl'); shutil.copy2(a.layout, inputs/'layout.json')
    roots = [Path(x).resolve() for x in a.deny_root]
    allowed_original = {Path(a.stl).resolve(), Path(a.layout).resolve()}
    extensions = {'.step', '.stp', '.brep', '.iges', '.igs', '.npz', '.npy', '.pkl', '.pickle', '.json'}
    forbidden = sorted({f.resolve() for root in roots for f in root.rglob('*')
                        if f.is_file() and f.suffix.lower() in extensions and not f.resolve().is_relative_to(out)
                        and f.resolve() not in allowed_original})
    probes = [str(Path(x).resolve()) for x in a.probe]
    if any(Path(x) not in forbidden for x in probes):
        raise ValueError('Every probe must be included in OS-locked files')
    manifest = dict(roots=[str(x) for x in roots], files=[str(x) for x in forbidden], probes=probes,
                    actual_data_inputs=['input/input.stl', 'input/layout.json'], controls=a.controls,
                    mechanism='CreateFileW GENERIC_READ, share mode 0; originals unmodified; all handles held until child exits')
    (out/'lock_manifest.json').write_text(json.dumps(manifest, indent=2, ensure_ascii=False), encoding='utf-8')
    worker = out/'worker.py'
    worker.write_text('''import json, os, sys, time
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
''', encoding='utf-8')
    create = ctypes.WinDLL('kernel32', use_last_error=True).CreateFileW
    create.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                       wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    create.restype = wintypes.HANDLE
    close = ctypes.WinDLL('kernel32', use_last_error=True).CloseHandle
    close.argtypes = [wintypes.HANDLE]; close.restype = wintypes.BOOL
    handles = []; started = time.perf_counter()
    try:
        for f in forbidden:
            h = create(str(f), 0x80000000, 0, None, 3, 0x80, None)
            if h == ctypes.c_void_p(-1).value:
                raise OSError(ctypes.get_last_error(), 'Could not acquire exclusive read handle', str(f))
            handles.append(h)
        env = dict(os.environ); env.pop('PYTHONPATH', None); env['PYTHONDONTWRITEBYTECODE'] = '1'
        with (out/'replay.log').open('w', encoding='utf-8') as log:
            proc = subprocess.run([sys.executable, '-I', str(worker)], cwd=out, env=env, stdout=log, stderr=subprocess.STDOUT)
        if proc.returncode:
            raise RuntimeError(f'Isolated replay failed; see {out}/replay.log')
    finally:
        for h in handles:
            close(h)
        (out/'parent_timing.json').write_text(json.dumps(dict(locked_file_count=len(handles),
                                                   wall_seconds=time.perf_counter()-started)), encoding='utf-8')


if __name__ == '__main__':
    main()
