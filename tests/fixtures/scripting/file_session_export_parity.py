"""#783, #784 and #775 through the agent surfaces.

#783: a `projectFile` session exports a movie through the MCP export tools exactly as the open
project does: the same file (byte for byte) as an export of the open project and as `bloom-cli
export --project --job`, with its job readable through export.wait and export.log, and a saved job
of the file started with export.start runs to its published file.

#784: the export-queue route (any render or export of a projectFile session, and the queue presets
of the open project) reports the same preservationReport as the scripted route (Render::run: MCP
render of the open project with a legacy preset, and bloom-cli render) for the same settings, with
the #760 background phrase.

#775: a call with `projectFile` set to the project the server has open attaches to it instead of
being refused, and says whether the open copy differs from the file and how to reload it.

Arguments: bloom-mcp, bloom-cli, and the staged Python package folder (unused).
"""
import hashlib
import json
import os
import pathlib
import shutil
import struct
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
MOVIE = 'builtin:prores-422-mov'
OPAQUE = 'background=composition (opaque over composition background)'


def digest(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


CONTAINERS = {b'moov', b'trak', b'mdia', b'minf', b'stbl', b'edts', b'dinf', b'udta', b'tref',
              b'mvex', b'moof', b'traf', b'gmhd', b'tapt'}


def box_path(data, begin, end, offset):
    """The QuickTime / ISO BMFF box path holding byte `offset` among the boxes in [begin, end)."""
    while begin + 8 <= end:
        size, kind = struct.unpack('>I4s', data[begin:begin + 8])
        header = 8
        if size == 1 and begin + 16 <= end:
            size, header = struct.unpack('>Q', data[begin + 8:begin + 16])[0], 16
        elif size == 0:
            size = end - begin
        if size < header or size > end - begin:
            return f'(unparsed at {begin})'
        if offset < begin + size:
            name = kind.decode('latin-1')
            if kind in CONTAINERS and offset >= begin + header:
                return name + '/' + box_path(data, begin + header, begin + size, offset)
            return f'{name} +{offset - begin}'
        begin += size
    return '(after the last box)'


def movie_difference(first, second):
    """Where two movie files differ, for a failure message; empty when they are the same bytes."""
    a, b = pathlib.Path(first).read_bytes(), pathlib.Path(second).read_bytes()
    if a == b:
        return ''
    common = min(len(a), len(b))
    differing = [i for i in range(common) if a[i] != b[i]]
    at = differing[0] if differing else common
    where = f' in {box_path(a, 0, len(a), at)}' if at < common else ''
    count = len(differing) + max(len(a), len(b)) - common
    return (f'{pathlib.Path(first).name} and {pathlib.Path(second).name}: sizes {len(a)} and '
            f'{len(b)}; first difference at byte {at}{where}; {count} bytes differ')


class Server:
    def __init__(self):
        self.process = stdio_child.start([MCP])
        self.next_id = 1
        self.call('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                                 'clientInfo': {'name': 'Bloom file session parity',
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

    def transact(self, *operations, **extra):
        revision = self.tool('query', {'kind': 'project', **extra})['revision']
        return self.tool('transact', {'expectedRevision': revision,
                                      'operations': list(operations), **extra})

    def close(self):
        stdio_child.end(self.process, 60)


class FileSessions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = Server()

    @classmethod
    def tearDownClass(cls):
        cls.server.close()

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='bloom-file-session-parity-')
        self.root = pathlib.Path(self.directory.name)
        server = self.server
        server.tool('project.new', {'discardChanges': True, 'stopJobs': True})
        made = server.transact(
            {'op': 'bloom.composition.set-duration', 'args': {'composition': 1,
                                                              'duration': FRAMES}},
            {'op': 'bloom.composition.set-format', 'args': {'composition': 1, 'width': 64,
                                                            'height': 36}},
            {'op': 'bloom.layer.add-solid', 'args': {'composition': 1, 'name': 'Card',
                                                     'color': [0.8, 0.2, 0.1, 1]}},
            {'op': 'bloom.export.add-job', 'args': {'composition': 1, 'outputs': [
                {'preset': MOVIE, 'destination': {'portability': 'project-relative',
                                                  'path': 'saved/shot.mov',
                                                  'overwrite': 'replace'}}]}})
        self.assertTrue(made['succeeded'], json.dumps(made))
        saved = server.tool('project.save-as', {'path': str(self.root / 'open.bloom')})
        self.assertNotIn('error', saved, json.dumps(saved))
        for name in ('file.bloom', 'cli.bloom'):
            shutil.copy(self.root / 'open.bloom', self.root / name)
        self.file = str(self.root / 'file.bloom')

    def tearDown(self):
        self.server.tool('project.close', {'projectFile': self.file, 'discardChanges': True,
                                           'stopJobs': True})
        self.server.tool('project.new', {'discardChanges': True, 'stopJobs': True})
        self.directory.cleanup()

    def export(self, name, **extra):
        return self.server.tool('export', {'composition': 1, 'preset': MOVIE,
                                           'destination': str(self.root / name),
                                           'overwrite': 'replace', **extra})

    def test_a_movie_from_a_file_session_matches_the_open_project_and_the_cli(self):
        live = self.export('live.mov')
        self.assertNotIn('error', live, json.dumps(live))
        filed = self.export('file.mov', projectFile=self.file)
        self.assertNotIn('error', filed, json.dumps(filed))
        cli = subprocess.run([CLI, 'export', '--project', str(self.root / 'cli.bloom'), '--all',
                              '--replace'], capture_output=True, text=True, timeout=300)
        self.assertEqual(cli.returncode, 0, cli.stdout + cli.stderr)
        movies = [self.root / 'live.mov', self.root / 'file.mov', self.root / 'saved' / 'shot.mov']
        for movie in movies:
            self.assertTrue(movie.is_file(), movie)
        # The same file from each route, byte for byte.
        self.assertEqual({digest(movie) for movie in movies}, {digest(movies[0])},
                         '; '.join(movie_difference(movies[0], movie) for movie in movies[1:]))
        for key in ('publishedFrames', 'streams', 'preservationReport'):
            self.assertEqual(filed[key], live[key], key)
        self.assertEqual(filed['publishedFrames'], FRAMES)
        self.assertEqual([path['digest'] for path in filed['files']],
                         ['sha256:' + digest(movies[1])])
        # The job of a file session answers export.wait and export.log with that projectFile.
        waited = self.server.tool('export.wait', {'projectFile': self.file, 'job': filed['job']})
        self.assertTrue(waited.get('finished'), json.dumps(waited))
        self.assertEqual(waited['job']['state'], 'done', json.dumps(waited))
        log = self.server.tool('export.log', {'projectFile': self.file, 'job': filed['job']})
        self.assertIn('export.output.published', [entry['code'] for entry in log['entries']],
                      json.dumps(log))
        # The open project's queue does not know the file session's job.
        status = self.server.tool('export.status', {'projectFile': self.file})
        self.assertEqual(status['runner'], 'idle', json.dumps(status))

    def test_a_saved_job_of_a_file_runs_through_export_start(self):
        status = self.server.tool('export.status', {'projectFile': self.file})
        saved = [job for job in status['jobs'] if not job.get('transient')]
        self.assertEqual(len(saved), 1, json.dumps(status))
        started = self.server.tool('export.start', {'projectFile': self.file,
                                                    'jobs': [saved[0]['id']]})
        self.assertTrue(started.get('accepted', False), json.dumps(started))
        waited = self.server.tool('export.wait', {'projectFile': self.file,
                                                  'job': saved[0]['id'], 'timeoutMs': 300000})
        self.assertTrue(waited.get('finished'), json.dumps(waited))
        self.assertEqual(waited['job']['state'], 'done', json.dumps(waited))
        published = self.root / 'saved' / 'shot.mov'
        self.assertTrue(published.is_file())
        through_mcp = digest(published)
        kept = self.root / 'through-mcp.mov'
        shutil.copy(published, kept)
        # bloom-cli runs the same saved job of an identical file to the same place.
        cli = subprocess.run([CLI, 'export', '--project', str(self.root / 'cli.bloom'), '--all',
                              '--replace'], capture_output=True, text=True, timeout=300)
        self.assertEqual(cli.returncode, 0, cli.stdout + cli.stderr)
        self.assertEqual(digest(published), through_mcp, movie_difference(kept, published))

    def test_the_queue_route_reports_what_the_scripted_route_reports(self):
        (self.root / 'scripted').mkdir()
        (self.root / 'queued').mkdir()
        scripted = self.server.tool('render', {
            'composition': 1, 'first': 0, 'last': 2, 'preset': 'PngRgba8SrgbV1',
            'destination': str(self.root / 'scripted' / 'seq.png'), 'overwrite': 'replace'})
        self.assertNotIn('error', scripted, json.dumps(scripted))
        queued = self.server.tool('render', {
            'projectFile': self.file, 'composition': 1, 'first': 0, 'last': 2,
            'preset': 'builtin:png-sequence', 'destination': str(self.root / 'queued' / 'seq.png'),
            'overwrite': 'replace'})
        self.assertNotIn('error', queued, json.dumps(queued))
        cli = subprocess.run([CLI, 'render', '--project', str(self.root / 'cli.bloom'),
                              '--range', '0-2', '--preset', 'PngRgba8SrgbV1', '--out',
                              str(self.root / 'cli'), '--replace'],
                             capture_output=True, text=True, timeout=300)
        self.assertEqual(cli.returncode, 0, cli.stdout + cli.stderr)
        report = scripted['preservationReport']
        self.assertTrue(report.startswith('preset=PngRgba8SrgbV1; exact-facets='), report)
        self.assertEqual(queued['preservationReport'], report)
        self.assertEqual(cli.stdout.splitlines()[0], report)
        # A movie through the queue names its preset and facets too.
        movie = self.export('report.mov', projectFile=self.file)
        self.assertTrue(movie['preservationReport'].startswith('preset=ProResMovV1; exact-facets='),
                        json.dumps(movie))
        # #760: the background phrase is kept on both routes.
        backed = self.server.tool('render', {
            'projectFile': self.file, 'composition': 1, 'frame': 0, 'preset': 'PngRgba8SrgbV1',
            'destination': str(self.root / 'backed.png'), 'overwrite': 'replace',
            'background': 'composition'})
        self.assertNotIn('error', backed, json.dumps(backed))
        self.assertTrue(backed['preservationReport'].startswith('preset=PngRgba8SrgbV1;'),
                        backed['preservationReport'])
        self.assertIn(OPAQUE, backed['preservationReport'])

    def test_the_open_project_file_attaches_instead_of_being_refused(self):
        opened = str(self.root / 'open.bloom')
        read = self.server.tool('query', {'kind': 'project', 'projectFile': opened})
        self.assertNotIn('error', read, json.dumps(read))
        self.assertTrue(read['attachedToOpenProject'], json.dumps(read))
        self.assertFalse(read['fileDiffers'], json.dumps(read))
        edited = self.server.transact(
            {'op': 'bloom.layer.add-solid', 'args': {'composition': 1, 'name': 'Attached',
                                                     'color': [0, 1, 0, 1]}},
            projectFile=opened)
        self.assertTrue(edited['succeeded'], json.dumps(edited))
        # The edit is in the open project itself, which now differs from its file.
        layers = self.server.tool('query', {'kind': 'layers', 'composition': 1})
        self.assertIn('Attached', json.dumps(layers))
        differs = self.server.tool('query', {'kind': 'project', 'projectFile': opened})
        self.assertTrue(differs['fileDiffers'], json.dumps(differs))
        self.assertEqual(differs['reload']['tool'], 'project.open')
        self.assertEqual(differs['reload']['arguments'], {'path': opened, 'discardChanges': True})
        saved = self.server.tool('project.save', {'projectFile': opened})
        self.assertNotIn('error', saved, json.dumps(saved))
        same = self.server.tool('query', {'kind': 'project', 'projectFile': opened})
        self.assertFalse(same['fileDiffers'], json.dumps(same))


if __name__ == '__main__':
    unittest.main(verbosity=2)
