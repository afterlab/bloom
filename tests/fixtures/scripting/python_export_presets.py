"""Every preset export.presets lists as available is accepted by Python and rendered by bloom-cli.

Issue #650: the preset list is read from `bloom-cli run` (the export.presets call), each available
preset becomes an export job through the Python operation with only its id, and `bloom-cli export`
renders the saved queue. A preset the list calls available must be accepted and must publish a
file; one it calls unavailable must carry a reason.
"""
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0, sys.argv.pop(1))
CLI = sys.argv.pop(1)
import _bloom
import bloom
from bloom import _state


class ExportPresets(unittest.TestCase):
    def setUp(self):
        self.host = _bloom.Host()
        self.token = _state.bind(self.host)

    def tearDown(self):
        _state._host.reset(self.token)

    def test_every_available_preset_is_accepted_and_rendered(self):
        with tempfile.TemporaryDirectory(prefix='bloom-export-presets-') as directory:
            folder = pathlib.Path(directory)
            script = folder / 'presets.json'
            script.write_text(json.dumps([{'op': 'export.presets', 'args': {}}]))
            listed = subprocess.run([CLI, 'run', str(script)], capture_output=True, timeout=120)
            self.assertEqual(listed.returncode, 0, listed.stderr.decode())
            presets = json.loads(listed.stdout.decode().splitlines()[0])
            ids = [preset['id'] for preset in presets]
            self.assertIn('builtin:h264-mp4', ids)
            self.assertEqual(len(ids), len(set(ids)))
            for preset in presets:
                self.assertTrue(preset['extension'], preset['id'])
                if not preset['available']:
                    self.assertTrue(preset['reason'], preset['id'])
            available = [preset for preset in presets if preset['available']]
            self.assertTrue(any(preset['id'] == 'builtin:h264-mp4' for preset in available),
                            'the H.264 MP4 preset is available where the media worker runs')

            # A new project uses Bloom Neutral: the presets that use ACES names are listed as
            # unavailable for it, with the reason. They are walked on an ACES project below.
            needs_aces = [preset for preset in presets
                          if not preset['available'] and 'ACES' in preset['reason']]
            self.assertTrue(needs_aces, 'the review presets need the ACES configuration')
            self.walk(folder / 'neutral', available)
            self.host.set_color_settings('aces')
            self.walk(folder / 'aces', needs_aces)

    def walk(self, folder, presets):
        """Adds a job per preset with only its id, saves, and renders the queue with bloom-cli."""
        folder.mkdir()
        composition = bloom.context.composition
        self.assertTrue(bloom.ops.layer.add_solid('Red', (1, 0, 0, 1))['succeeded'])
        expected = []
        for index, preset in enumerate(presets):
            name = '%s-%02d' % (folder.name, index)
            path = 'Exports/%s/out.%s' % (name, preset['extension'])
            output = {'preset': preset['id'],
                      'destination': {'portability': 'project-relative', 'path': path,
                                      'overwrite': 'replace'}}
            added = bloom.ops.export.add_job(name=name, range='custom', first=0, last=1,
                                             outputs=(json.dumps(output),),
                                             composition=composition)
            self.assertTrue(added['succeeded'], preset['id'])
            expected.append((preset['id'], name, folder / 'Exports' / name))

        project = folder / 'shot.bloom'
        self.host.save(str(project))
        command = subprocess.run([CLI, 'export', '--project', str(project), '--all'],
                                 capture_output=True, timeout=540)
        report = command.stdout.decode()
        failed = [report[max(0, at - 200):at + 1200]
                  for at in range(len(report)) if report.startswith('"type":"failed"', at)]
        self.assertEqual(command.returncode, 0,
                         command.stderr.decode() + '\n'.join(failed or [report[-4000:]]))
        for preset, name, published in expected:
            files = [item for item in published.glob('*') if item.stat().st_size > 0]
            self.assertTrue(files, '%s published nothing: %s' % (preset, report[-2000:]))

if __name__ == '__main__':
    unittest.main()
