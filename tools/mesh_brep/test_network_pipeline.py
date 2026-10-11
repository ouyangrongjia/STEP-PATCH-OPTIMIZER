import json
from dataclasses import asdict
from pathlib import Path
import tempfile
import unittest
from .network_pipeline import retain_changed_objective
from .global_continuity import default_config,OBJECTIVE_VERSION


class ObjectiveResumeTests(unittest.TestCase):
    def test_changed_objective_preserves_old_delivery_and_discards_stale_downstream_cache(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary); (root/'continuity').mkdir()
            (root/'continuity/global_continuity.json').write_text(json.dumps({'config':{'seam_weight':.2}}))
            (root/'validated/step').mkdir(parents=True)
            (root/'validated/step/model.step').write_bytes(b'previous evaluated geometry')
            (root/'final_metrics').mkdir()
            (root/'final_metrics/baseline_continuity.json').write_bytes(b'{"baseline":true}')
            (root/'final_metrics/final_continuity.json').write_bytes(b'{"old_final":true}')
            (root/'network_01').mkdir(); (root/'network_01/input.npz').write_bytes(b'unchanged input')
            result=retain_changed_objective(root,root/'baseline')
            archive=Path(result['archive'])
            self.assertEqual((archive/'validated/step/model.step').read_bytes(),b'previous evaluated geometry')
            self.assertFalse((root/'validated').exists())
            self.assertFalse((root/'final_metrics/final_continuity.json').exists())
            self.assertEqual((root/'final_metrics/baseline_continuity.json').read_bytes(),b'{"baseline":true}')
            self.assertEqual((root/'network_01/input.npz').read_bytes(),b'unchanged input')
            (root/'continuity').mkdir()
            (root/'continuity/global_continuity.json').write_text(json.dumps(dict(
                config=asdict(default_config()),reference_fit=str((root/'baseline').resolve()),
                objective_version=OBJECTIVE_VERSION)))
            self.assertIsNone(retain_changed_objective(root,root/'baseline'))


if __name__=='__main__':
    unittest.main()
