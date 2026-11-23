"""#790 through every agent surface: a source file that changed, moved or was deleted stops a render or
export with the same sentence and the same classification.

Python render: `diagnostic_code` is "media-source-unavailable". bloom-cli render: the error line is
`error[bloom-cli.render.media-source-unavailable]`; bloom-cli export: the error line is
`error[bloom-cli.export.media-source-unavailable]`. The stdio MCP export: `ExportFailed` with the
detail `export.media.unavailable`. Each sentence names the file, says to restore or relink it, and
never calls the stop a bug in Bloom. Nothing is published.

Arguments: bloom-mcp, bloom-cli, and the staged Python package folder.
"""
import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stdio_child  # noqa: E402

MCP = sys.argv.pop(1)
CLI = sys.argv.pop(1)
PYTHON = sys.argv.pop(1)

FRAMES = 4
MOVIE = 'builtin:prores-422-mov'


def write_png(path, rgb):
    def chunk(kind, data):
        body = kind + data
        return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body))
    raw = (b'\x00' + bytes(rgb) * 16) * 8
    header = struct.pack('>IIBBBBB', 16, 8, 8, 2, 0, 0, 0)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) +
                     chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
    return path


def outputs(result, name):
    return [output['id'] for output in result['outputs'] if output['name'] == name]


def assert_plain_sentence(case, text, what):
    case.assertIn('"plate"', text, what + ' names the file')
    case.assertTrue('relink' in text and 'Restore' in text, what + ' says restore or relink: ' + text)
    case.assertNotIn('bug in Bloom', text, what)


class Server:
    def __init__(self):
        self.process = stdio_child.start([MCP])
        self.next_id = 1
        self.call('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                                 'clientInfo': {'name': 'Bloom source unavailable parity',
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


def change(plate, how):
    """The picture changes (rewritten with other pixels) or goes away after it was imported."""
    if how == 'removed':
        plate.unlink()
    else:
        write_png(plate, (10, 200, 30))


class Mcp(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = Server()

    @classmethod
    def tearDownClass(cls):
        cls.server.close()

    def test_a_changed_or_removed_source_stops_the_export_with_its_sentence(self):
        for how in ('changed', 'removed'):
            with self.subTest(how), tempfile.TemporaryDirectory(prefix='bloom-source-mcp-') as folder:
                root = pathlib.Path(folder)
                plate = write_png(root / 'plate.png', (200, 30, 10))
                self.server.tool('project.new', {'discardChanges': True, 'stopJobs': True})
                made = self.server.transact(
                    {'op': 'bloom.composition.set-duration', 'args': {'composition': 1,
                                                                      'duration': FRAMES}},
                    {'op': 'bloom.composition.set-format', 'args': {'composition': 1,
                                                                    'width': 64, 'height': 36}},
                    {'op': 'bloom.asset.import', 'args': {'paths': [str(plate)],
                                                          'projectDirectory': str(root)}})
                self.assertTrue(made['succeeded'], json.dumps(made))
                added = self.server.transact({'op': 'bloom.layer.add-from-assets',
                                              'args': {'assets': outputs(made, 'asset'),
                                                       'time': 0}})
                self.assertTrue(added['succeeded'], json.dumps(added))
                change(plate, how)
                movie = root / 'shot.mov'
                failed = self.server.tool('export', {'composition': 1, 'preset': MOVIE,
                                                     'destination': str(movie),
                                                     'overwrite': 'replace'})
                text = json.dumps(failed)
                self.assertFalse(failed.get('succeeded', True), text)
                self.assertEqual(failed['error']['structuredContent']['diagnostic']['code'],
                                 'bloom.mcp.export-failed')
                self.assertEqual(failed['error']['structuredContent']['diagnostic']['detail'],
                                 'export.media.unavailable')
                assert_plain_sentence(self, failed['error']['structuredContent']['diagnostic']['message'],
                                      'the MCP export')
                self.assertNotIn('bug in Bloom', text)
                self.assertFalse(movie.exists(), 'nothing is published')


class Python(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, PYTHON)
        global _bloom, bloom, _state  # noqa: PLW0603
        import _bloom
        import bloom
        from bloom import _state

    def prepare(self):
        self.host = _bloom.Host()
        self.token = _state.bind(self.host)
        self.addCleanup(_state._host.reset, self.token)
        directory = tempfile.TemporaryDirectory(prefix='bloom-source-python-')
        self.addCleanup(directory.cleanup)
        self.root = pathlib.Path(directory.name)
        self.plate = write_png(self.root / 'plate.png', (200, 30, 10))
        self.assertTrue(bloom.ops.composition.set_duration(FRAMES)['succeeded'])
        self.assertTrue(bloom.ops.composition.set_format(width=64, height=36)['succeeded'])
        imported = bloom.ops.asset.import_((str(self.plate),), str(self.root))
        self.assertTrue(imported['succeeded'], imported)
        added = bloom.ops.layer.add_from_assets(outputs(imported, 'asset'), time=0)
        self.assertTrue(added['succeeded'], added)

    def check(self, how):
        self.prepare()
        change(self.plate, how)
        out = self.root / 'shots'
        out.mkdir()
        result = self.host.render(int(bloom.context.composition), 0, FRAMES - 1, True,
                                  'PngRgba8SrgbV1', str(out / 'shot.png'), 0, True, False,
                                  bloom.render._subtitles(None), 'transparent')
        self.assertFalse(result['succeeded'], result)
        self.assertEqual(result['diagnostic_code'], 'media-source-unavailable', result)
        assert_plain_sentence(self, result['diagnostic'], 'the Python render')
        self.assertEqual([p for p in out.rglob('*') if p.is_file()], [], 'nothing is published')
        with self.assertRaises(RuntimeError) as raised:
            bloom.render.range(out=out / 'again.png', replace=True)
        assert_plain_sentence(self, str(raised.exception), 'bloom.render.range')

    def test_render_of_a_changed_source_reports_the_media_source_class(self):
        self.check('changed')

    def test_render_of_a_removed_source_reports_the_media_source_class(self):
        self.check('removed')


class Cli(unittest.TestCase):
    def test_render_and_export_say_the_source_is_unavailable(self):
        # A saved project's file read afresh by a new process takes the file's current pixels, so only a
        # moved or deleted file stops it; the in-memory sessions above also catch a changed one.
        for how in ('removed',):
            with self.subTest(how), tempfile.TemporaryDirectory(prefix='bloom-source-cli-') as folder:
                root = pathlib.Path(folder)
                plate = write_png(root / 'plate.png', (200, 30, 10))
                server = Server()
                try:
                    made = server.transact(
                        {'op': 'bloom.composition.set-duration',
                         'args': {'composition': 1, 'duration': FRAMES}},
                        {'op': 'bloom.composition.set-format',
                         'args': {'composition': 1, 'width': 64, 'height': 36}},
                        {'op': 'bloom.asset.import', 'args': {'paths': [str(plate)],
                                                              'projectDirectory': str(root)}})
                    self.assertTrue(made['succeeded'], json.dumps(made))
                    added = server.transact({'op': 'bloom.layer.add-from-assets',
                                             'args': {'assets': outputs(made, 'asset'),
                                                      'time': 0}})
                    self.assertTrue(added['succeeded'], json.dumps(added))
                    job = server.transact({'op': 'bloom.export.add-job', 'args': {
                        'composition': 1, 'outputs': [{
                            'preset': MOVIE, 'destination': {'portability': 'project-relative',
                                                             'path': 'saved/shot.mov',
                                                             'overwrite': 'replace'}}]}})
                    self.assertTrue(job['succeeded'], json.dumps(job))
                    saved = server.tool('project.save-as', {'path': str(root / 'scene.bloom')})
                    self.assertNotIn('error', saved, json.dumps(saved))
                finally:
                    server.close()
                change(plate, how)
                render = subprocess.run([CLI, 'render', '--project', str(root / 'scene.bloom'),
                                         '--preset', 'PngRgba8SrgbV1', '--out',
                                         str(root / 'shots' / 'shot.png')],
                                        capture_output=True, text=True, timeout=180)
                self.assertNotEqual(render.returncode, 0, render.stdout)
                self.assertIn('error[bloom-cli.render.media-source-unavailable]', render.stderr)
                assert_plain_sentence(self, render.stderr, 'bloom-cli render')
                self.assertEqual([p for p in (root / 'shots').rglob('*') if p.is_file()], [],
                                 'nothing is published')
                export = subprocess.run([CLI, 'export', '--project', str(root / 'scene.bloom'),
                                         '--all', '--replace'],
                                        capture_output=True, text=True, timeout=180)
                self.assertNotEqual(export.returncode, 0, export.stdout)
                failed = [event for event in json.loads(export.stdout)['events']
                          if event.get('type') == 'failed']
                self.assertEqual(len(failed), 1, export.stdout)
                assert_plain_sentence(self, failed[0]['reason'], 'bloom-cli export')
                # The same class as the render line and MCP's export.media.unavailable, with the
                # failed event's sentence.
                lines = [line for line in export.stderr.splitlines() if line.startswith('error[')]
                self.assertEqual(
                    lines, ['error[bloom-cli.export.media-source-unavailable]: ' +
                            failed[0]['reason']], export.stderr)
                self.assertNotIn('bug in Bloom', export.stdout + export.stderr)
                self.assertFalse((root / 'saved' / 'shot.mov').exists(), 'nothing is published')


if __name__ == '__main__':
    unittest.main()
