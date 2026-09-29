"""Fresh fixed holdouts for frozen candidates; no fit or selection imports."""
import argparse
from pathlib import Path
from .evaluate import evaluate_mesh


def main():
    p = argparse.ArgumentParser(); p.add_argument('--run', required=True); p.add_argument('--group', choices=['candidate', 'fixtures'], required=True)
    a = p.parse_args(); r = Path(a.run)
    if a.group == 'candidate':
        cases = [('baseline_cold', 'baseline_cold/aligned_inner_cubic/patch_faces.step', 'candidate_coarse'),
                 ('A/inverse_xyz', 'A/inverse_xyz/network.step', 'candidate_coarse'),
                 ('A/mesh_uv_xyz', 'A/mesh_uv_xyz/network.step', 'candidate_coarse'),
                 ('B_coarse_29_global', 'B_coarse_29_global/network.step', 'candidate_coarse'),
                 ('B_coarse_25', 'B_coarse_25/network.step', 'candidate_coarse'),
                 ('B_fine_25', 'B_fine_25/network.step', 'candidate_fine')]
        source = r/'baseline_cold/original_region.step'
    else:
        cases = [(name, name+'/network.step', mesh) for name, mesh in [
            ('B_closed32_13_global', 'closed32'), ('B_closed64_13', 'closed64'),
            ('B_wrapped32_17', 'wrapped32'), ('B_wrapped64_17', 'wrapped64')]]
        source = None
    for name, step, mesh in cases:
        out = r/name/'confirmation_v2.json'
        # A resumable driver never overwrites a completed confirmation.
        if out.exists():
            continue
        evaluate_mesh(r/'inputs'/mesh/'input.stl', r/step, out, 30000, 2909280, 12000, source)


if __name__ == '__main__':
    main()
