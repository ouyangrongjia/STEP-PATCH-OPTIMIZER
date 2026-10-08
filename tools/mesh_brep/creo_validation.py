"""Independent Creo import validation; no reconstruction engine dependency."""
import json
import os
import re
import shutil
import subprocess
from pathlib import Path
import xml.etree.ElementTree as ET
from .mesh import dump


def export_native_part(part, output, creo_root, format='step', timeout=480):
    """Export independent Creo diagnostic evidence without modifying the part."""
    out = Path(output).resolve()
    if out.exists():
        raise FileExistsError(out)
    runner = Path(__file__).with_name('creo_export.ps1')
    log_path = out.with_name(out.name+'_runner.log')
    with log_path.open('w', encoding='utf-8') as log:
        process = subprocess.run([shutil.which('pwsh') or 'powershell.exe', '-NoProfile', '-File', str(runner),
            '-Part', str(Path(part).resolve()), '-OutputDir', str(out), '-CreoRoot', str(creo_root),
            '-Format', format, '-TimeoutSeconds', str(timeout)], stdout=log, stderr=subprocess.STDOUT,
            timeout=timeout+60, creationflags=subprocess.CREATE_NO_WINDOW)
    if process.returncode:
        raise RuntimeError(f'Independent Creo export failed: {log_path}')
    return out/('model.stp' if format == 'step' else 'model.jpg')


def native_body_check(part, output, creo_root, probe, timeout=240):
    """Inspect native bodies, closed envelopes and quilt/body surface identity."""
    out = Path(output).resolve(); out.mkdir(parents=True, exist_ok=False)
    common = Path(creo_root).resolve()/'Common Files'; env = os.environ.copy()
    env['PRO_COMM_MSG_EXE'] = str(common/'x86e_win64/obj/pro_comm_msg.exe')
    env['PATH'] = os.pathsep.join([str(common/'bin'), str(common/'x86e_win64/lib'),
                                 str(common/'protoolkit/x86e_win64/obj'), env.get('PATH', '')])
    command = f'"{Path(creo_root).resolve()}/Parametric/bin/parametric.bat" -g:no_graphics -i:rpc_input'
    with (out/'runner.log').open('w', encoding='utf-8') as log:
        result = subprocess.run([str(Path(probe).resolve()), command, str(Path(part).resolve()), str(out/'run.json')],
            cwd=out, env=env, stdout=log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW,
            timeout=timeout)
    if result.returncode:
        raise RuntimeError('Native Creo body inspection failed; see runner.log and raw API status')
    return json.loads((out/'run.json').read_text(encoding='utf-8'))


def creo_check(step, output, creo_root, timeout=300, boundary_preference='UV', profile_mode='preserve_splines'):
    """Use a task-local import profile and read actual ModelCHECK facts."""
    root = Path(output).resolve(); root.mkdir(parents=True, exist_ok=False)
    template = Path(creo_root)/'Common Files/x86e_win64/mworks/mw_import_mm.dip'
    profile = template.read_text(encoding='utf-8')
    if profile_mode not in ('preserve_splines', 'installed'):
        raise ValueError('Unknown Creo diagnostic import profile mode')
    if boundary_preference not in ('UV', 'XYZ'):
        raise ValueError('Creo boundary preference must be UV or XYZ')
    settings = dict(ModelAccuracy='External', UseImptTemplate='1', KeepBSplineSrf='1',
                    SimplifySurface='0', ArclenCrvReparam='0', SurfBndryPref=boundary_preference, EnableATB='0')
    for key, value in (settings.items() if profile_mode == 'preserve_splines' else []):
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
                  profile=str(root/'import_mm.dip'), report=str(reports[0]), gray_display_observed=False,
                  boundary_preference=re.search(r'\bSurfBndryPref="([^"]*)"', profile).group(1),
                  profile_mode=profile_mode, profile_modified=profile_mode != 'installed',
                  profile_source=str(template.resolve()))
    result['import_validation_passed'] = result['part_status'] == 'OK' and result['score'] == 'PASS'
    # Import validation is not a native-body query. Kept as a separate gate;
    # callers can combine this report with native_body_check and visual review.
    result['solid_import_passed'] = None
    result['native_body_check_required'] = True
    dump(root/'run.json', result); return result
