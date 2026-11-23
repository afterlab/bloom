"""#766 and #769 through every agent surface.

#766: a render or export without `first`/`last` covers the composition's whole range, and giving one
of them takes the composition's own range for the other. MCP render and export, bloom.render.range
(Python) and bloom-cli render give the same frames.

#769: bloom.export.add-job takes its `outputs` as objects over MCP and Python (and as JSON text), and
a refusal names the output that failed.

Arguments: bloom-mcp, bloom-cli, and the staged Python package folder.
"""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stdio_child  # noqa: E402

MCP = sys.argv.pop(1)
CLI = sys.argv.pop(1)
PYTHON = sys.argv.pop(1)

FRAMES = 6
OUTPUTS = [
    {'preset': 'builtin:png-sequence',
     'destination': {'portability': 'project-relative', 'path': 'Exports/a/{name}.png',
                     'overwrite': 'replace'}},
    {'preset': 'builtin:png-sequence',
     'destination': {'portability': 'project-relative', 'path': 'Exports/b/{name}.png',
                     'overwrite': 'replace'}},
]


class Server:
    def __init__(self):
        self.process = stdio_child.start([MCP])
        self.next_id = 1
        self.call('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                                 'clientInfo': {'name': 'Bloom export arguments parity',
                                                'version': '1'}})
        self.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})

    def send(self, message):
        self.process.stdin.write((json.dumps(message) + '\n').encode())
        self.process.stdin.flush()

    def call(self, method, params):
        self.next_id += 1
        self.send({'jsonrpc': '2.0', 'id': self.next_id, 'method': method, 'params': params})
        line = self.process.stdout.readline()
        if not line:
            raise AssertionError(stdio_child.errors(self.process))
        return json.loads(line)

    def tool(self, name, arguments):
        response = self.call('tools/call', {'name': name, 'arguments': arguments})
        if 'error' in response:
            return {'succeeded': False, 'error': response['error']}
        result = response['result']
        if result.get('isError'):
            return {'succeeded': False, 'error': result}
        return result.get('structuredContent', result)

    def transact(self, *operations):
        revision = self.tool('query', {'kind': 'project'})['revision']
        return self.tool('transact', {'expectedRevision': revision,
                                      'operations': list(operations)})

    def close(self):
        stdio_child.end(self.process, 60)


def sequence_files(folder):
    return sorted(path.name for path in pathlib.Path(folder).glob('*.png'))


class Mcp(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = Server()

    @classmethod
    def tearDownClass(cls):
        cls.server.close()

    def setUp(self):
        self.server.tool('project.new', {'discardChanges': True, 'stopJobs': True})
        changed = self.server.transact(
            {'op': 'bloom.composition.set-duration', 'args': {'composition': 1,
                                                              'duration': FRAMES}},
            {'op': 'bloom.composition.set-format', 'args': {'composition': 1, 'width': 64,
                                                            'height': 36}})
        self.assertTrue(changed['succeeded'], json.dumps(changed))
        self.directory = tempfile.TemporaryDirectory(prefix='bloom-export-arguments-')
        self.root = pathlib.Path(self.directory.name)

    def tearDown(self):
        self.directory.cleanup()

    def render(self, name, **extra):
        (self.root / name).mkdir()
        return self.server.tool('render', {
            'composition': 1, 'preset': 'PngRgba8SrgbV1', 'overwrite': 'replace',
            'destination': str(self.root / name / 'shot.png'), **extra})

    def test_render_without_a_range_covers_the_composition(self):
        whole = self.render('all')
        self.assertNotIn('error', whole, json.dumps(whole))
        self.assertEqual(whole['publishedFrames'], FRAMES, json.dumps(whole))
        self.assertEqual(len(sequence_files(self.root / 'all')), FRAMES)

    def test_one_end_takes_the_composition_range_for_the_other(self):
        tail = self.render('tail', first=2)
        self.assertEqual(tail['publishedFrames'], FRAMES - 2, json.dumps(tail))
        head = self.render('head', last=1)
        self.assertEqual(head['publishedFrames'], 2, json.dumps(head))
        self.assertEqual(len(sequence_files(self.root / 'head')), 2)

    def test_a_frame_and_a_range_together_are_still_refused(self):
        refused = self.render('both', frame=0, first=1)
        self.assertFalse(refused.get('succeeded', True), json.dumps(refused))

    def test_export_of_a_movie_without_a_range(self):
        exported = self.server.tool('export', {
            'composition': 1, 'preset': 'builtin:prores-422-mov', 'overwrite': 'replace',
            'destination': str(self.root / 'movie.mov')})
        self.assertNotIn('error', exported, json.dumps(exported))
        self.assertEqual(exported['publishedFrames'], FRAMES, json.dumps(exported))
        self.assertTrue((self.root / 'movie.mov').is_file())

    def test_help_says_so(self):
        text = json.dumps(self.server.tool('help', {'name': 'export'}))
        self.assertIn('giving neither first nor last exports the whole composition', text)

    def test_add_job_takes_outputs_as_objects(self):
        added = self.server.transact({'op': 'bloom.export.add-job',
                                      'args': {'composition': 1, 'range': 'composition',
                                               'outputs': OUTPUTS}})
        self.assertTrue(added['succeeded'], json.dumps(added))
        jobs = self.server.tool('export.status', {})['jobs']
        self.assertEqual([len(job['outputs']) for job in jobs], [2], json.dumps(jobs)[:2000])

    def test_add_job_takes_outputs_as_json_text(self):
        added = self.server.transact({'op': 'bloom.export.add-job',
                                      'args': {'composition': 1, 'range': 'composition',
                                               'outputs': [json.dumps(OUTPUTS[0]), OUTPUTS[1]]}})
        self.assertTrue(added['succeeded'], json.dumps(added))
        jobs = self.server.tool('export.status', {})['jobs']
        self.assertEqual([len(job['outputs']) for job in jobs], [2])

    def test_add_job_error_names_the_output(self):
        refused = self.server.transact({'op': 'bloom.export.add-job',
                                        'args': {'composition': 1, 'range': 'composition',
                                                 'outputs': [OUTPUTS[0], {'preset': 'nope'}]}})
        self.assertFalse(refused['succeeded'], json.dumps(refused))
        self.assertIn('outputs[1]', json.dumps(refused))
        not_an_object = self.server.transact({'op': 'bloom.export.add-job',
                                              'args': {'composition': 1,
                                                       'outputs': [OUTPUTS[0], 7]}})
        self.assertFalse(not_an_object.get('succeeded', False), json.dumps(not_an_object))
        self.assertIn('outputs[1]', json.dumps(not_an_object))

    def test_add_output_takes_an_object(self):
        added = self.server.transact({'op': 'bloom.export.add-job',
                                      'args': {'composition': 1, 'range': 'composition',
                                               'outputs': [OUTPUTS[0]]}})
        job = next(item['id'] for item in added['outputs'] if item['name'] == 'exportJob')
        extra = self.server.transact({'op': 'bloom.export.add-output',
                                      'args': {'job': job, 'output': OUTPUTS[1]}})
        self.assertTrue(extra['succeeded'], json.dumps(extra))


class Python(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, PYTHON)
        global _bloom, bloom, _state  # noqa: PLW0603
        import _bloom
        import bloom
        from bloom import _state

    def setUp(self):
        self.host = _bloom.Host()
        self.token = _state.bind(self.host)
        self.assertTrue(bloom.ops.composition.set_duration(FRAMES)['succeeded'])
        self.assertTrue(bloom.ops.composition.set_format(width=64, height=36)['succeeded'])
        self.directory = tempfile.TemporaryDirectory(prefix='bloom-export-arguments-python-')
        self.root = pathlib.Path(self.directory.name)

    def tearDown(self):
        _state._host.reset(self.token)
        self.directory.cleanup()

    def destination(self, name):
        (self.root / name).mkdir()
        return self.root / name / 'shot.png'

    def test_range_without_ends_covers_the_composition(self):
        whole = bloom.render.range(out=self.destination('all'))
        self.assertEqual(whole.published_frames, FRAMES)
        self.assertEqual(len(whole.files), FRAMES)
        tail = bloom.render.range(first=2, out=self.destination('tail'))
        self.assertEqual(tail.published_frames, FRAMES - 2)
        head = bloom.render.range(last=1, out=self.destination('head'))
        self.assertEqual(head.published_frames, 2)
        self.assertEqual(len(sequence_files(self.root / 'head')), 2)
        with self.assertRaises(ValueError):
            bloom.render.range(first=3, last=1, out=self.destination('backwards'))

    def test_add_job_takes_outputs_as_dicts(self):
        added = bloom.ops.export.add_job(composition=bloom.context.composition,
                                         range='composition', outputs=tuple(OUTPUTS))
        self.assertTrue(added['succeeded'], added)
        saved = self.root / 'shot.bloom'
        self.host.save(str(saved))
        # Both outputs publish when the job runs: the saved queue carries two of them.
        command = subprocess.run([CLI, 'export', '--project', str(saved), '--all'],
                                 capture_output=True, timeout=180)
        self.assertEqual(command.returncode, 0, command.stderr.decode())
        self.assertEqual(len(sequence_files(self.root / 'Exports' / 'a')), FRAMES)
        self.assertEqual(len(sequence_files(self.root / 'Exports' / 'b')), FRAMES)

    def test_add_job_names_the_output_that_failed(self):
        with self.assertRaises(bloom.errors.OperationError) as refused:
            bloom.ops.export.add_job(composition=bloom.context.composition,
                                     outputs=(OUTPUTS[0], {'preset': 'nope'}))
        self.assertIn('outputs[1]', str(refused.exception))

    def test_cli_render_without_a_range(self):
        project = self.root / 'scene.bloom'
        self.host.save(str(project))
        base = [CLI, 'render', '--project', str(project), '--preset', 'PngRgba8SrgbV1']
        whole = subprocess.run(base + ['--out', str(self.root / 'cli-all')],
                               capture_output=True, timeout=180)
        self.assertEqual(whole.returncode, 0, whole.stderr.decode())
        self.assertEqual(len(sequence_files(self.root / 'cli-all')), FRAMES)
        tail = subprocess.run(base + ['--range', '2-', '--out', str(self.root / 'cli-tail')],
                              capture_output=True, timeout=180)
        self.assertEqual(tail.returncode, 0, tail.stderr.decode())
        self.assertEqual(len(sequence_files(self.root / 'cli-tail')), FRAMES - 2)
        head = subprocess.run(base + ['--range', '-1', '--out', str(self.root / 'cli-head')],
                              capture_output=True, timeout=180)
        self.assertEqual(head.returncode, 0, head.stderr.decode())
        self.assertEqual(len(sequence_files(self.root / 'cli-head')), 2)
        both = subprocess.run(base + ['--frame', '0', '--range', '1-2',
                                      '--out', str(self.root / 'cli-both')],
                              capture_output=True, timeout=60)
        self.assertEqual(both.returncode, 2)


if __name__ == '__main__':
    unittest.main()
