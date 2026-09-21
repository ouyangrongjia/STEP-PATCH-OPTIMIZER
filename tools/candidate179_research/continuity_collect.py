"""Collect existing numerical evidence; never change thresholds or fit geometry."""
import argparse
import csv
import json
from pathlib import Path
from geometry import dump,read_step,topology


def read(path):return json.loads(path.read_text(encoding='utf-8-sig'))


def cad(record):
    return [dict(object=Path(c['object']).name,checks={x['name']:x['answer'] for x in c['summary'].get('key_checks',[])},
        diagnostic_passed=c['summary'].get('diagnostic_passed')) for c in record.get('component_checks',[])]


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);args=parser.parse_args();out=Path(args.run);protocol=read(out/'protocol.json')
    boundary=read(out/'boundary_audit.json')
    if any('control' not in e['variants'] for e in boundary['edges']):
        from designer_audit import original_boundaries
        original_boundaries(out,protocol,{'control':read_step(out/'control/patch_faces.step')},{'control':read(out/'control/fit.json')})
        boundary=read(out/'boundary_audit.json')
    rows=[];names=['control']+[c['name'] for c in protocol['configurations']]
    names += sorted(p.name for p in out.iterdir() if p.is_dir() and (p.name.endswith('_initial_center') or p.name.endswith('_initial_solver')))
    for name in names:
        target=out/name
        if not (target/'fit.json').exists() or not (target/'evaluation.json').exists():continue
        fit=read(target/'fit.json');evaluation=read(target/'evaluation.json')
        g1=[r['variants'][name]['adjacent_normal_degrees']['max'] for r in boundary['edges'] if r['requested_continuity']=='G1' and name in r['variants']]
        row=dict(name=name,phase='superseded_implementation' if '_initial_' in name else 'current',cad_faces=fit['after_read']['faces'],control_points=fit['actual_control_points'],
            max_direction_poles=max(max(p['complexity']['u_poles'],p['complexity']['v_poles']) for p in fit['patches']),
            forward_max_mm=evaluation['distance_mm']['max'],forward_rms_mm=evaluation['distance_mm']['rms'],forward_p95_mm=evaluation['distance_mm']['p95'],
            reverse_max_mm=evaluation['reverse']['distance_mm']['max'],normal_p95_deg=evaluation['normal_degrees']['p95'],normal_max_deg=evaluation['normal_degrees']['max'],
            knot_jump_max_deg=max((k['normal_jump_degrees']['max'] for k in evaluation['knot_jumps']),default=0.),
            seam_normal_max_deg=max(s['normal_degrees']['max'] for s in fit['seams']),outer_g1_max_deg=max(g1) if g1 else None,
            local_brep_valid=fit['after_read']['brep_valid'],fit_pipeline_seconds=fit['seconds'],can_apply=False)
        row.update(whole_solids=None,whole_free_edges=None,whole_brep_valid=None,creo_geom_checks=None)
        if (target/'integration.json').exists():
            topo=read(target/'integration.json')['after_step_read'];row.update(whole_solids=topo['solids'],whole_free_edges=topo['free_edges'],whole_brep_valid=topo['brep_valid'])
        elif name=='control':
            # Reread the archived control; do not infer validity from the record.
            path=target/'whole_topology_reread.json'
            if not path.exists():dump(path,topology(read_step(target/'whole_model_diagnostic.step')))
            topo=read(path);row.update(whole_solids=topo['solids'],whole_free_edges=topo['free_edges'],whole_brep_valid=topo['brep_valid'])
        if (target/'creo/result.json').exists():
            records=cad(read(target/'creo/result.json'))
            if len(records)==1:row['creo_geom_checks']=int(records[0]['checks']['GEOM_CHECKS'])
        rows.append(row)
    with (out/'comparison.csv').open('w',encoding='utf-8',newline='') as file:
        writer=csv.DictWriter(file,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
    cad_rows=[]
    for path in sorted(out.rglob('result.json')):
        if 'creo' not in str(path.relative_to(out)):continue
        record=read(path);cad_rows.append(dict(result=str(path.relative_to(out)),status=record['status'],components=cad(record)))
    dump(out/'cad_summary.json',cad_rows)
    selected=next(r for r in rows if r['name']=='aligned_inner_cubic');geometry=read(out/'geometry_validation.json');interface=read(out/'aligned_inner_cubic/integrated_boundary_audit.json')
    confirmation=read(out/'confirmation.json');local=read(out/'local_evaluation.json');remaining=read(out/'confirmed_worst_local_evaluation.json')
    measured_normal_max=max(selected['normal_max_deg'],confirmation['aligned_inner_cubic']['normal_degrees']['max'],local['aligned_inner_cubic']['normal_degrees']['max'],remaining['aligned_inner_cubic']['normal_degrees']['max'])
    creo_path=out/'aligned_inner_cubic/creo/result.json'
    result=dict(run_id=out.name,research_outcome='A: improved diagnostic candidate',selected_candidate='aligned_inner_cubic',
        status='DiagnosticOnly',can_apply=False,direct_control='five_knot_smooth at 3c810bfe07b2396c36e8338a255c9516524be83e',
        input_source=read(out/'input_audit.json'),dependencies=read(out/'dependencies.json'),source_state=read(out/'source_state.json'),
        new_fit_configurations=sum(not c.get('verification_only',False) for c in protocol['configurations']),
        verification_configurations=sum(c.get('verification_only',False) for c in protocol['configurations']),
        fit_execution_records=len(rows)-1,
        comparison=rows,selected_checks=dict(exact_outer_curve_representations=geometry['aligned_inner_cubic']['original_edge_identity']['all_curve_representations_equal'],
            original_edge_tolerances_equal=geometry['aligned_inner_cubic']['original_edge_identity']['all_edge_tolerances_equal'],
            whole_step_valid=selected['whole_brep_valid'],interface_after_step_passed=interface['passed'],
            distance_gate_mm=protocol['gates']['distance_mm'],fixed_sample_bidirectional_distance_passed=max(selected['forward_max_mm'],selected['reverse_max_mm'])<=protocol['gates']['distance_mm'],
            normal_gate_degrees=protocol['gates']['normal_degrees'],normal_gate_passed=measured_normal_max<=protocol['gates']['normal_degrees'],
            direction_control_point_budget=protocol['gates']['max_control_points_per_direction'],complexity_gate_passed=selected['max_direction_poles']<=protocol['gates']['max_control_points_per_direction'],
            creo_geom_checks=selected['creo_geom_checks'],creo_modelcheck_passed=read(creo_path)['validated'] if creo_path.exists() else None,native_cpp_strict_topology_gate='Not run',product_apply='Not run'),
        independent_confirmation=read(out/'confirmation_sampling.json'),confirmation=confirmation,
        reverse_confirmation=read(out/'reverse_confirmation.json') if (out/'reverse_confirmation.json').exists() else None,
        local_confirmation=local,remaining_worst_region=remaining,
        limitations=['Finite sampling maxima, not continuous-domain deviation bounds.','Numerical tangent-plane equations do not establish strict C1/G1.','Research prototypes only; product C++ and existing 49-to-46 regression were not changed or rerun.'])
    dump(out/'run.json',result);print('Collected',len(rows),'fit records;',len(cad_rows),'raw Creo report groups',flush=True)


if __name__=='__main__':main()
