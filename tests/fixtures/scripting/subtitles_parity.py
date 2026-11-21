"""#752: the subtitle operations and the `subtitles` read through every agent surface. MCP (the
stdio server), Python (bloom.ops.subtitles and bloom.data.subtitles) and `bloom-cli run` build the
same lists from the same arguments, take the same time forms (frames, [n, d] seconds and
"HH:MM:SS.mmm"), read data files (fromFile), and refuse the same subtitle limits naming the bound.

The #752 export settings travel the same way: the `subtitles` argument of render and export
(MCP), bloom.render's subtitles (Python), bloom-cli render and export --subtitles, an output's
saved settings, and the script export.script writes.

Arguments: bloom-mcp, bloom-cli, and the staged Python package folder (or "-" without Python).
"""
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from fractions import Fraction

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stdio_child  # noqa: E402

MCP = sys.argv.pop(1)
CLI = sys.argv.pop(1)
PYTHON = sys.argv.pop(1)

SRT = ("1\n00:00:01,000 --> 00:00:02,000\nOne\n\n"
       "2\n00:00:02,500 --> 00:00:03,000\nTwo\n")
# An ASS event with \move, which Bloom drops and reports.
ASS = ("[Script Info]\nScriptType: v4.00+\nPlayResX: 384\nPlayResY: 288\n\n"
       "[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, Bold, Italic, Alignment\n"
       "Style: Default,Arial,20,&H00FFFFFF,0,0,2\n\n"
       "[Events]\nFormat: Layer, Start, End, Style, Text\n"
       "Dialogue: 0,0:00:01.00,0:00:02.00,Default,{\\move(0,0,10,10)}Moving\n")


def sha256(path):
    return 'sha256:' + hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def settings_json(subtitles=None):
    """A complete PNG sequence output settings object, with the optional subtitles setting."""
    settings = {'format': 'png', 'codecProfile': '', 'compression': '', 'outputColorSpace': '',
                'display': '', 'view': '', 'bakeLook': True, 'includeAudio': False,
                'audioSampleRate': 48000, 'encoder': 'automatic',
                'naming': {'framePadding': 4, 'pattern': '<base>.####.<ext>'}}
    if subtitles is not None:
        settings['subtitles'] = subtitles
    return settings


def png_output(path, subtitles=None):
    return json.dumps({'preset': 'builtin:png-sequence', 'settings': settings_json(subtitles),
                       'destination': {'portability': 'project-relative', 'path': path,
                                       'overwrite': 'replace'},
                       'enabled': True, 'importIntoAssets': False})


def outputs(result, name):
    return [output['id'] for output in result['outputs'] if output['name'] == name]


class Server:
    """One stdio server kept open, so each call can read the previous call's result."""

    def __init__(self):
        self.process = stdio_child.start([MCP])
        self.next_id = 1
        self.initialized = self.call('initialize', {
            'protocolVersion': '2025-11-25', 'capabilities': {},
            'clientInfo': {'name': 'Bloom subtitles parity', 'version': '1'}})['result']
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
        return response['result']['structuredContent']

    def revision(self):
        return self.tool('query', {'kind': 'project'})['revision']

    def transact(self, *operations):
        return self.tool('transact', {'expectedRevision': self.revision(),
                                      'operations': list(operations)})

    def subtitles(self, **arguments):
        return self.tool('query', {'kind': 'subtitles', 'composition': 1, **arguments})

    def close(self):
        stdio_child.end(self.process, 60)


def plain_cues(read):
    """A read's cues as (id, start seconds, end seconds, text): what every surface must agree on."""
    return [(cue['id'], tuple(cue['start']['seconds']), tuple(cue['end']['seconds']), cue['text'])
            for cue in read['cues']]


class Mcp(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = Server()

    @classmethod
    def tearDownClass(cls):
        cls.server.close()

    def setUp(self):
        self.server.tool('project.new', {'discardChanges': True, 'stopJobs': True})
        self.directory = tempfile.TemporaryDirectory(prefix='bloom-subtitles-parity-')
        self.root = pathlib.Path(self.directory.name)

    def tearDown(self):
        self.directory.cleanup()

    def add(self, cues):
        result = self.server.transact({'op': 'bloom.layer.add-subtitles',
                                       'args': {'composition': 1, 'cues': cues}})
        self.assertTrue(result['succeeded'], json.dumps(result))
        return outputs(result, 'subtitlesNode')[0], outputs(result, 'cue')

    def test_limits_are_advertised(self):
        limits = self.server.initialized['capabilities']['experimental']['bloom']['limits']
        self.assertEqual(limits['subtitleCues'], 50000)
        self.assertEqual(limits['subtitleCueTextBytes'], 4096)
        self.assertEqual(limits['subtitleFileBytes'], 16 * 1024 * 1024)
        self.assertEqual(limits['subtitleStyles'], 256)
        self.assertEqual(limits['subtitleActiveCues'], 16)

    def test_time_forms_and_the_read(self):
        rate = self.server.tool('query', {'kind': 'compositions'})['records'][0]['frameRate']
        node, cues = self.add([{'start': '00:00:01.000', 'end': rate[0] * 2 // rate[1],
                                'text': 'One'},
                               {'start': [5, 2], 'end': '00:00:03,000',
                                'text': 'Two <b>bold</b>', 'speaker': 'Ada'}])
        self.assertEqual(cues, [1, 2])
        read = self.server.subtitles(node=node)
        self.assertEqual(read['count'], 2)
        self.assertEqual(plain_cues(read), [(1, (1, 1), (2, 1), 'One'),
                                            (2, (5, 2), (3, 1), 'Two <b>bold</b>')])
        self.assertEqual(read['cues'][1]['plainText'], 'Two bold')
        self.assertEqual(read['cues'][1]['speaker'], 'Ada')
        self.assertEqual(read['cues'][0]['timestamp'], '00:00:01.000 --> 00:00:02.000')
        self.assertEqual(read['cues'][1]['duration']['seconds'], [1, 2])
        self.assertEqual(read['provenance'], {'kind': 'authored'})
        self.assertEqual(read['linkedFile'], 'none')
        # Filters: the cue showing at 2.6 s, and the cues overlapping [0, 2).
        self.assertEqual([cue['id'] for cue in self.server.subtitles(node=node, at=[13, 5])['cues']],
                         [2])
        self.assertEqual([cue['id'] for cue in
                          self.server.subtitles(node=node, **{'from': 0, 'to': '00:00:02.000'})[
                              'cues']], [1])
        # Paging.
        first = self.server.subtitles(node=node, pageSize=1)
        self.assertEqual([cue['id'] for cue in first['cues']], [1])
        second = self.server.subtitles(node=node, pageSize=1, cursor=first['nextCursor'])
        self.assertEqual([cue['id'] for cue in second['cues']], [2])
        self.assertIsNone(second['nextCursor'])
        # As SRT and WebVTT text.
        srt = self.server.subtitles(node=node, format='srt')
        self.assertTrue(srt['text'].startswith('1\n00:00:01,000 --> 00:00:02,000\nOne\n'),
                        srt['text'])
        self.assertTrue(self.server.subtitles(node=node, format='vtt')['text'].startswith('WEBVTT'))
        # Without a node, the composition's Subtitles nodes.
        listed = self.server.subtitles()
        self.assertEqual(listed['nodes'], [{'node': node, 'count': 2, 'language': 'und',
                                            'provenance': 'authored'}])

    def test_edits_keep_ids_and_report_them(self):
        node, _ = self.add([{'start': 24, 'end': 48, 'text': 'One'},
                            {'start': 72, 'end': 96, 'text': 'Two'}])
        read = self.server.subtitles(node=node)
        kept = read['cues'][0]
        set_cues = self.server.transact({'op': 'bloom.subtitles.set-cues', 'args': {
            'composition': 1, 'node': node,
            'cues': [{'id': kept['id'], 'start': kept['start']['seconds'],
                      'end': kept['end']['seconds'], 'text': 'One'},
                     {'start': '00:00:05.000', 'end': '00:00:06.000', 'text': 'Three'}]}})
        self.assertTrue(set_cues['succeeded'], json.dumps(set_cues))
        self.assertEqual(outputs(set_cues, 'cue'), [3])
        self.assertEqual(outputs(set_cues, 'removedCue'), [2])
        # Events report the changed cue ids.
        events = self.server.tool('events', {'sinceRevision': set_cues['revision'] - 1})
        names = [(output['name'], output['id']) for event in events['events']
                 for output in event.get('outputs', [])]
        self.assertIn(('cue', 3), names)
        self.assertIn(('removedCue', 2), names)
        # One undo step.
        self.server.tool('history', {'action': 'undo', 'expectedRevision': self.server.revision()})
        self.assertEqual([cue['id'] for cue in self.server.subtitles(node=node)['cues']], [1, 2])
        self.server.tool('history', {'action': 'redo', 'expectedRevision': self.server.revision()})

        edited = self.server.transact({'op': 'bloom.subtitles.edit-cue', 'args': {
            'composition': 1, 'node': node, 'cue': 1, 'text': 'First', 'end': '00:00:02.500'}})
        self.assertEqual(outputs(edited, 'cue'), [1])
        inserted = self.server.transact({'op': 'bloom.subtitles.insert-cue', 'args': {
            'composition': 1, 'node': node, 'start': [1, 2], 'end': [3, 4], 'text': 'Zero'}})
        self.assertEqual(outputs(inserted, 'cue'), [4])
        shifted = self.server.transact({'op': 'bloom.subtitles.shift-cues', 'args': {
            'composition': 1, 'node': node, 'by': '-00:00:00.500'}})
        self.assertTrue(shifted['succeeded'], json.dumps(shifted))
        scaled = self.server.transact({'op': 'bloom.subtitles.scale-cues', 'args': {
            'composition': 1, 'node': node, 'factor': [2, 1], 'cues': [4]}})
        self.assertTrue(scaled['succeeded'], json.dumps(scaled))
        self.assertEqual(plain_cues(self.server.subtitles(node=node)),
                         [(4, (0, 1), (1, 2), 'Zero'), (1, (1, 2), (2, 1), 'First'),
                          (3, (9, 2), (11, 2), 'Three')])
        removed = self.server.transact({'op': 'bloom.subtitles.remove-cues', 'args': {
            'composition': 1, 'node': node, 'indices': [0]}})
        self.assertEqual(outputs(removed, 'removedCue'), [4])
        early = self.server.transact({'op': 'bloom.subtitles.shift-cues', 'args': {
            'composition': 1, 'node': node, 'by': -48}})
        self.assertFalse(early['succeeded'])
        self.assertIn('before 0', early['diagnostics'][0]['message'])

    def test_load_link_reload_unlink_and_write(self):
        node, _ = self.add([])
        warned = self.server.transact({'op': 'bloom.subtitles.load', 'args': {
            'composition': 1, 'node': node, 'text': ASS}})
        self.assertTrue(warned['succeeded'], json.dumps(warned))
        self.assertEqual(len(outputs(warned, 'cue')), 1, 'cueCount is the cue outputs')
        warnings = [item['message'] for item in warned['diagnostics']
                    if item['code'] == 'bloom.scripting.operation-warning']
        self.assertTrue(any(text.startswith('line 12: [dropped] \\move') for text in warnings),
                        warnings)
        strict = self.server.transact({'op': 'bloom.subtitles.load', 'args': {
            'composition': 1, 'node': node, 'text': ASS, 'strict': True}})
        self.assertFalse(strict['succeeded'])

        source = self.root / 'episode.srt'
        source.write_bytes(SRT.encode())
        linked = self.server.transact({'op': 'bloom.subtitles.load', 'args': {
            'composition': 1, 'node': node, 'file': str(source), 'link': True}})
        self.assertTrue(linked['succeeded'], json.dumps(linked))
        read = self.server.subtitles(node=node)
        self.assertEqual(read['provenance']['kind'], 'file')
        self.assertEqual(read['provenance']['format'], 'srt')
        self.assertEqual(read['linkedFile'], 'in-sync')
        ids = [cue['id'] for cue in read['cues']]
        refused = self.server.transact({'op': 'bloom.subtitles.edit-cue', 'args': {
            'composition': 1, 'node': node, 'index': 0, 'text': 'Here'}})
        self.assertFalse(refused['succeeded'])
        self.assertIn('Unlink', refused['diagnostics'][0]['message'])

        source.write_bytes(SRT.replace('Two', 'Deux').encode())
        self.assertEqual(self.server.subtitles(node=node)['linkedFile'], 'changed')
        reloaded = self.server.transact({'op': 'bloom.subtitles.reload', 'args': {
            'composition': 1, 'node': node}})
        self.assertTrue(reloaded['succeeded'], json.dumps(reloaded))
        read = self.server.subtitles(node=node)
        self.assertEqual(read['linkedFile'], 'in-sync')
        self.assertEqual(read['cues'][0]['id'], ids[0], 'the unchanged cue keeps its id')
        self.assertEqual(read['cues'][1]['text'], 'Deux')

        unlinked = self.server.transact({'op': 'bloom.subtitles.unlink', 'args': {
            'composition': 1, 'node': node}})
        self.assertTrue(unlinked['succeeded'], json.dumps(unlinked))
        self.assertEqual(self.server.subtitles(node=node)['provenance'], {'kind': 'authored'})

        target = self.root / 'out.vtt'
        wrote = self.server.transact({'op': 'bloom.subtitles.write-file', 'args': {
            'composition': 1, 'node': node, 'path': str(target), 'format': 'vtt'}})
        self.assertTrue(wrote['succeeded'], json.dumps(wrote))
        self.assertTrue(target.read_text().startswith('WEBVTT\n'))
        again = self.server.transact({'op': 'bloom.subtitles.write-file', 'args': {
            'composition': 1, 'node': node, 'path': str(target), 'format': 'vtt'}})
        self.assertFalse(again['succeeded'])
        self.assertIn('overwrite', again['diagnostics'][0]['message'])
        replaced = self.server.transact({'op': 'bloom.subtitles.write-file', 'args': {
            'composition': 1, 'node': node, 'path': str(target), 'format': 'srt',
            'overwrite': True}})
        self.assertTrue(replaced['succeeded'], json.dumps(replaced))
        self.assertTrue(target.read_text().startswith('1\n'))

    def test_from_file_and_limit_refusals(self):
        node, _ = self.add([])
        data = self.root / 'cues.json'
        data.write_text(json.dumps([{'start': index * 2, 'end': index * 2 + 1,
                                     'text': f'Cue {index}'} for index in range(200)]))
        loaded = self.server.transact({'op': 'bloom.subtitles.set-cues', 'args': {
            'composition': 1, 'node': node, 'cues': {'fromFile': str(data)}}})
        self.assertTrue(loaded['succeeded'], json.dumps(loaded)[:400])
        self.assertEqual(self.server.subtitles(node=node)['count'], 200)

        def refusal(args):
            result = self.server.transact({'op': 'bloom.subtitles.set-cues',
                                           'args': {'composition': 1, 'node': node, **args}})
            self.assertFalse(result['succeeded'])
            return result['diagnostics'][0]

        data.write_text(json.dumps([{'start': index, 'end': index + 1, 'text': 'x'}
                                    for index in range(50001)]))
        cues = refusal({'cues': {'fromFile': str(data)}})
        self.assertEqual(cues['code'], 'bloom.mcp.limit.subtitle-cues')
        self.assertEqual(cues['limit'], {'name': 'subtitleCues', 'value': 50000, 'actual': 50001,
                                         'unit': 'cues'})
        text = refusal({'cues': [{'start': 0, 'end': 1, 'text': 'x' * 4097}]})
        self.assertEqual(text['code'], 'bloom.mcp.limit.subtitle-cue-text-bytes')
        self.assertEqual(text['limit'], {'name': 'subtitleCueTextBytes', 'value': 4096,
                                         'actual': 4097, 'unit': 'bytes'})
        styles = refusal({'cues': [], 'styles': [{'name': f's{index}'} for index in range(257)]})
        self.assertEqual(styles['code'], 'bloom.mcp.limit.subtitle-styles')
        self.assertEqual(styles['limit']['actual'], 257)
        large = self.root / 'large.srt'
        with large.open('wb') as stream:
            stream.truncate(16 * 1024 * 1024 + 1)
        result = self.server.transact({'op': 'bloom.subtitles.load', 'args': {
            'composition': 1, 'node': node, 'file': str(large)}})
        self.assertFalse(result['succeeded'])
        self.assertEqual(result['diagnostics'][0]['code'], 'bloom.mcp.limit.subtitle-file-bytes')
        self.assertEqual(result['diagnostics'][0]['limit'],
                         {'name': 'subtitleFileBytes', 'value': 16777216, 'actual': 16777217,
                          'unit': 'bytes'})


    def test_render_and_export_deliver_sidecars(self):
        node, _ = self.add([{'start': 0, 'end': '00:00:00.500', 'text': 'Hello'},
                            {'start': '00:00:00.500', 'end': '00:00:01.000', 'text': 'World'}])
        frames = self.root / 'frames.png'
        # Formats may come in any order; the result lists the tracks in format order.
        rendered = self.server.tool('render', {
            'composition': 1, 'first': 0, 'last': 2, 'preset': 'PngRgba8SrgbV1',
            'destination': str(frames), 'overwrite': 'replace',
            'subtitles': {'sidecar': ['vtt', 'srt'], 'nodes': [node]}})
        self.assertNotIn('error', rendered, json.dumps(rendered))
        tracks = rendered['subtitles']
        self.assertEqual([track['format'] for track in tracks], ['srt', 'vtt'])
        listed = {item['path']: item['digest'] for item in rendered['files']}
        for track in tracks:
            self.assertEqual(track['node'], node)
            self.assertEqual(track['cues'], 1)  # frames 0-2 show only the first cue
            self.assertEqual(track['bytes'], os.path.getsize(track['path']))
            self.assertEqual(track['sha256'], sha256(track['path']))
            self.assertEqual(listed[track['path']], sha256(track['path']))
            self.assertIn('Hello', pathlib.Path(track['path']).read_text())
        self.assertEqual(len(rendered['files']), 3 + 2)
        # A render without the argument delivers none.
        plain = self.server.tool('render', {
            'composition': 1, 'first': 0, 'last': 0, 'preset': 'PngRgba8SrgbV1',
            'destination': str(self.root / 'plain.png'), 'overwrite': 'replace'})
        self.assertNotIn('subtitles', plain)
        self.assertEqual(len(plain['files']), 1)
        # Export takes the same argument for a movie.
        exported = self.server.tool('export', {
            'composition': 1, 'first': 0, 'last': 11, 'preset': 'builtin:prores-422hq-mov',
            'destination': str(self.root / 'movie.mov'), 'overwrite': 'replace',
            'subtitles': {'sidecar': ['ttml', 'srt']}})
        self.assertNotIn('error', exported, json.dumps(exported))
        self.assertEqual([track['format'] for track in exported['subtitles']], ['srt', 'ttml'])
        digests = {item['path']: item['digest'] for item in exported['files']}
        for track in exported['subtitles']:
            self.assertEqual(digests[track['path']], sha256(track['path']))
            self.assertEqual(track['sha256'], sha256(track['path']))
        self.assertEqual({item['path'] for item in exported['files']},
                         {str(self.root / 'movie.mov')} |
                         {track['path'] for track in exported['subtitles']})

    def test_subtitles_argument_is_checked(self):
        self.add([{'start': 0, 'end': '00:00:01.000', 'text': 'Hello'}])
        destination = str(self.root / 'refused.mov')
        def refused(subtitles, preset='builtin:prores-422hq-mov', tool='export', **extra):
            response = self.server.tool(tool, {
                'composition': 1, 'frame': 0, 'preset': preset, 'destination': destination,
                'subtitles': subtitles, **extra})
            self.assertIn('error', response, json.dumps(subtitles))
            self.assertEqual(response['error']['code'], -32602, json.dumps(response))
            return response['error']['message']
        self.assertIn('srt, vtt and ttml', refused({'sidecar': ['ass']}))
        self.assertIn('at most once', refused({'sidecar': ['srt', 'srt']}))
        self.assertIn('sidecar format', refused({}))
        self.assertIn('sidecar format', refused({'sidecar': []}))
        self.assertIn('sidecar format', refused({'embed': False}))
        self.assertIn('Unknown member', refused({'sidecar': ['srt'], 'format': 'srt'}))
        self.assertIn('1 to 256', refused({'sidecar': ['srt'], 'nodes': list(range(1, 258))}))
        self.assertIn('1 to 256', refused({'sidecar': ['srt'], 'nodes': []}))
        self.assertIn('positive', refused({'sidecar': ['srt'], 'nodes': [0]}))
        self.assertIn('at most once', refused({'sidecar': ['srt'], 'nodes': [3, 3]}))
        refused('srt')
        # A container with no subtitle track refuses embed, naming the sidecar formats.
        for preset in ('builtin:dnxhr-hq-mxf', 'builtin:wav', 'builtin:png-sequence'):
            self.assertIn('SRT, WebVTT or TTML sidecar files',
                          refused({'sidecar': ['srt'], 'embed': True}, preset=preset))
        self.assertIn('SRT, WebVTT or TTML sidecar files',
                      refused({'embed': True}, preset='PngRgba8SrgbV1', tool='render'))
        # Nothing was written by a refused call.
        self.assertEqual(list(self.root.glob('*')), [])

    def test_export_script_reproduces_the_subtitles_setting(self):
        node, _ = self.add([{'start': 0, 'end': '00:00:01.000', 'text': 'Hello'}])
        setting = {'sidecar': ['srt', 'vtt'], 'embed': False, 'nodes': [str(node)]}
        added = self.server.transact({'op': 'bloom.export.add-job', 'args': {
            'composition': 1, 'range': 'frame', 'first': 0,
            'outputs': [png_output('Exports/{name}.png', setting)]}})
        self.assertTrue(added['succeeded'], json.dumps(added))
        job = outputs(added, 'exportJob')[0]
        status = self.server.tool('export.status', {})
        view = next(item for item in status['jobs'] if item['id'] == str(job))
        self.assertEqual(view['outputs'][0]['subtitles'],
                         {'sidecar': ['srt', 'vtt'], 'embed': False, 'nodes': [node]})
        for form in ('python', 'cli-json'):
            script = self.server.tool('export.script', {'job': job, 'format': form})['script']
            self.assertIn('subtitles', script, form)
            self.assertIn('sidecar', script, form)
            self.assertIn('vtt', script, form)
        script = self.server.tool('export.script', {'job': job, 'format': 'cli-json'})['script']
        self.assertIn('\\"sidecar\\":[\\"srt\\",\\"vtt\\"],\\"embed\\":false', script)
        # Running the generated script (without its start) recreates the setting.
        recreated = json.loads(self.server.tool(
            'export.script', {'job': job, 'format': 'cli-json'})['script'])
        recreated = [operation for operation in recreated if operation['op'] != 'export.start']
        recreated.append({'op': 'export.status', 'args': {}})
        with tempfile.TemporaryDirectory(prefix='bloom-subtitles-script-') as directory:
            script = pathlib.Path(directory) / 'script.json'
            script.write_text(json.dumps(recreated))
            run = subprocess.run([CLI, 'run', str(script)], capture_output=True, timeout=120)
        self.assertEqual(run.returncode, 0, run.stderr.decode())
        self.assertIn('"subtitles":{"sidecar":["srt","vtt"],"embed":false,"nodes":[',
                      run.stdout.decode())


@unittest.skipIf(PYTHON == '-', 'built without Python')
class Python(unittest.TestCase):
    """The same steps from Python: bloom.ops.subtitles and bloom.data.subtitles."""

    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, PYTHON)
        global _bloom, bloom, _state, OperationError  # noqa: PLW0603
        import _bloom
        import bloom
        from bloom import _state
        from bloom.errors import OperationError

    def setUp(self):
        self.host = _bloom.Host()
        self.token = _state.bind(self.host)
        self.directory = tempfile.TemporaryDirectory(prefix='bloom-subtitles-python-')
        self.root = pathlib.Path(self.directory.name)

    def tearDown(self):
        _state._host.reset(self.token)
        self.directory.cleanup()

    def test_time_forms_edits_and_read(self):
        added = bloom.ops.layer.add_subtitles(cues=(
            {'start': '00:00:01.000', 'end': Fraction(2), 'text': 'One'},
            {'start': 2.5, 'end': (3, 1), 'text': 'Two <b>bold</b>', 'speaker': 'Ada'}))
        node = outputs(added, 'subtitlesNode')[0]
        self.assertEqual(outputs(added, 'cue'), [1, 2])
        read = bloom.data.subtitles(node)
        self.assertEqual(plain_cues(read), [(1, (1, 1), (2, 1), 'One'),
                                            (2, (5, 2), (3, 1), 'Two <b>bold</b>')])
        self.assertEqual(read['cues'][1]['plain_text'], 'Two bold')
        self.assertEqual(read['linked_file'], 'none')
        self.assertEqual([cue['id'] for cue in bloom.data.subtitles(node, at=Fraction(13, 5))[
            'cues']], [2])
        set_cues = bloom.ops.subtitles.set_cues(node, (
            {'id': 1, 'start': (1, 1), 'end': (2, 1), 'text': 'One'},
            {'start': '00:00:05.000', 'end': '00:00:06.000', 'text': 'Three'}))
        self.assertEqual(outputs(set_cues, 'cue'), [3])
        self.assertEqual(outputs(set_cues, 'removedCue'), [2])
        bloom.transactions.undo()
        self.assertEqual([cue['id'] for cue in bloom.data.subtitles(node)['cues']], [1, 2])
        bloom.transactions.redo()
        bloom.ops.subtitles.edit_cue(node, cue=1, text='First', end='00:00:02.500')
        bloom.ops.subtitles.shift_cues(node, by='-00:00:00.500')
        self.assertEqual(plain_cues(bloom.data.subtitles(node)),
                         [(1, (1, 2), (2, 1), 'First'), (3, (9, 2), (11, 2), 'Three')])
        self.assertTrue(bloom.data.subtitles(node, format='srt')['text'].startswith('1\n'))
        self.assertEqual(bloom.data.subtitles()['nodes'][0]['node'], node)

    def test_load_write_and_limits(self):
        node = outputs(bloom.ops.layer.add_subtitles(), 'subtitlesNode')[0]
        source = self.root / 'episode.srt'
        source.write_bytes(SRT.encode())
        loaded = bloom.ops.subtitles.load(node, file=str(source), link=True)
        self.assertEqual(len(outputs(loaded, 'cue')), 2)
        self.assertEqual(bloom.data.subtitles(node)['linked_file'], 'in-sync')
        bloom.ops.subtitles.unlink(node)
        target = self.root / 'out.srt'
        bloom.ops.subtitles.write_file(node, str(target), 'srt')
        self.assertEqual(target.read_text(), SRT)
        with self.assertRaises(OperationError) as raised:
            bloom.ops.subtitles.set_cues(node, ({'start': 0, 'end': 1, 'text': 'x' * 4097},))
        detail = raised.exception.diagnostics[0]
        self.assertEqual(dict(detail['limit']), {'name': 'subtitleCueTextBytes', 'value': 4096,
                                                 'actual': 4097})

    def add_cues(self):
        added = bloom.ops.layer.add_subtitles(cues=(
            {'start': 0, 'end': '00:00:00.500', 'text': 'Hello'},
            {'start': '00:00:00.500', 'end': '00:00:01.000', 'text': 'World'}))
        return outputs(added, 'subtitlesNode')[0]

    def test_render_delivers_sidecars(self):
        node = self.add_cues()
        result = bloom.render.range(0, 2, out=self.root / 'frames.png',
                                    subtitles={'sidecar': ['vtt', 'srt'], 'nodes': [node]})
        self.assertEqual([track.format for track in result.subtitles], ['srt', 'vtt'])
        digests = {item.path: item.digest for item in result.files}
        for track in result.subtitles:
            self.assertEqual((track.node, track.cues), (node, 1))  # frames 0-2: the first cue
            self.assertEqual(track.bytes, os.path.getsize(track.path))
            self.assertEqual(track.sha256, sha256(track.path))
            self.assertEqual(digests[track.path], sha256(track.path))
            self.assertIn('Hello', pathlib.Path(track.path).read_text())
        self.assertEqual(len(result.files), 3 + 2)
        # One frame, and no subtitles at all.
        single = bloom.render.frame(0, out=self.root / 'one.png', subtitles={'sidecar': ['ttml']})
        self.assertEqual([track.format for track in single.subtitles], ['ttml'])
        plain = bloom.render.frame(0, out=self.root / 'plain.png')
        self.assertEqual((plain.subtitles, len(plain.files)), ((), 1))

    def test_render_refuses_a_bad_subtitles_dict(self):
        self.add_cues()
        for subtitles in ({'sidecar': ['ass']}, {'sidecar': ['srt', 'srt']}, {}, {'sidecar': []},
                          {'embed': False}, {'sidecar': ['srt'], 'format': 'srt'},
                          {'sidecar': ['srt'], 'nodes': []}, {'sidecar': ['srt'], 'nodes': [0]},
                          {'sidecar': ['srt'], 'nodes': [2, 2]}, 'srt', ['srt'],
                          {'sidecar': ['srt'], 'nodes': list(range(1, 258))}):
            with self.assertRaises(ValueError, msg=repr(subtitles)):
                bloom.render.frame(0, out=self.root / 'bad.png', subtitles=subtitles)
        # An image carries no track: embed names the sidecar formats.
        with self.assertRaises(RuntimeError) as raised:
            bloom.render.frame(0, out=self.root / 'embed.png',
                               subtitles={'sidecar': ['srt'], 'embed': True})
        self.assertIn('SRT, WebVTT or TTML sidecar files', str(raised.exception))
        self.assertEqual(list(self.root.glob('*')), [])

    def saved_project(self, subtitles=None):
        """A saved project with the cues and one PNG output export job."""
        node = self.add_cues()
        setting = None if subtitles is None else {**subtitles, 'nodes': [str(node)]}
        added = bloom.ops.export.add_job(composition=bloom.context.composition, range='frame',
                                         first=0, outputs=(png_output('Exports/{name}.png',
                                                                     setting),))
        self.assertTrue(added['succeeded'], added)
        project = self.root / 'shot.bloom'
        self.host.save(str(project))
        return project

    def test_a_saved_output_setting_round_trips_and_exports(self):
        project = self.saved_project({'sidecar': ['srt', 'vtt'], 'embed': False})
        command = subprocess.run([CLI, 'export', '--project', str(project), '--all'],
                                 capture_output=True, timeout=180)
        self.assertEqual(command.returncode, 0, command.stderr.decode())
        report = json.loads(command.stdout.decode().splitlines()[-1])
        delivered = [track for event in report['events'] for output in event.get('outputs', [])
                     for track in output.get('subtitles', [])]
        self.assertEqual(sorted(track['format'] for track in delivered), ['srt', 'vtt'], report)
        for track in delivered:
            self.assertEqual('sha256:' + track['sha256'], sha256(track['path']))
            self.assertEqual(track['cues'], 1)  # the one frame shows the first cue
        self.assertEqual(len(list((self.root / 'Exports').glob('*.srt'))), 1)
        self.assertEqual(len(list((self.root / 'Exports').glob('*.vtt'))), 1)

    def test_cli_export_subtitles_option(self):
        project = self.saved_project()
        refused = subprocess.run([CLI, 'export', '--project', str(project), '--all',
                                  '--subtitles', 'srt,ass'], capture_output=True, timeout=60)
        self.assertEqual(refused.returncode, 2)
        self.assertIn(b'error[bloom-cli.subtitles]', refused.stderr)
        for arguments in (['--subtitles', 'srt,srt'], ['--subtitle-nodes', '3'],
                          ['--subtitles', 'srt', '--subtitle-nodes', '0'],
                          ['--subtitles', 'srt', '--subtitle-nodes', '4,4']):
            result = subprocess.run([CLI, 'export', '--project', str(project), '--all'] + arguments,
                                    capture_output=True, timeout=60)
            self.assertEqual(result.returncode, 2, arguments)
            self.assertIn(b'error[bloom-cli.subtitles]', result.stderr, arguments)
        self.assertEqual(list((self.root / 'Exports').glob('*')) if (self.root / 'Exports').exists()
                         else [], [])
        # vtt,srt in any order; the output had no subtitles setting.
        command = subprocess.run([CLI, 'export', '--project', str(project), '--all',
                                  '--subtitles', 'vtt,srt,ttml'],
                                 capture_output=True, timeout=180)
        self.assertEqual(command.returncode, 0, command.stderr.decode())
        for extension in ('srt', 'vtt', 'ttml'):
            self.assertEqual(len(list((self.root / 'Exports').glob('*.' + extension))), 1,
                             extension)
        # The saved project is untouched: a run without the option writes no sidecar.
        for sidecar in (self.root / 'Exports').glob('*.*t*'):
            if sidecar.suffix != '.png':
                sidecar.unlink()
        again = subprocess.run([CLI, 'export', '--project', str(project), '--all', '--replace'],
                               capture_output=True, timeout=180)
        self.assertEqual(again.returncode, 0, again.stderr.decode())
        self.assertEqual([item for item in (self.root / 'Exports').glob('*')
                          if item.suffix != '.png'], [])
        # --save writes back the settings as they were saved, not the run's subtitles.
        before = project.read_bytes()
        saved = subprocess.run([CLI, 'export', '--project', str(project), '--all', '--replace',
                                '--save', '--subtitles', 'srt'], capture_output=True, timeout=180)
        self.assertEqual(saved.returncode, 0, saved.stderr.decode())
        self.assertEqual(len(list((self.root / 'Exports').glob('*.srt'))), 1)
        self.assertNotIn(b'"subtitles"', project.read_bytes())
        self.assertEqual(project.read_bytes(), before)

    def test_cli_render_subtitles_option(self):
        self.add_cues()
        project = self.root / 'scene.bloom'
        self.host.save(str(project))
        render = [CLI, 'render', '--project', str(project), '--range', '0-2', '--preset',
                  'PngRgba8SrgbV1']
        command = subprocess.run(render + ['--out', str(self.root / 'frames'),
                                           '--subtitles', 'vtt,srt'],
                                 capture_output=True, timeout=180)
        self.assertEqual(command.returncode, 0, command.stderr.decode())
        lines = [line for line in command.stdout.decode().splitlines()
                 if line.startswith('subtitles node=')]
        self.assertEqual(len(lines), 2, command.stdout.decode())
        self.assertIn('format=srt', lines[0])
        self.assertIn('format=vtt', lines[1])
        for line, extension in zip(lines, ('srt', 'vtt')):
            fields = dict(item.split('=', 1) for item in line.split()[1:])
            self.assertEqual(fields['cues'], '1')  # frames 0-2 show the first cue only
            self.assertEqual('sha256:' + fields['sha256'], sha256(fields['path']))
            self.assertTrue(fields['path'].endswith('.' + extension), line)
        # No option, no sidecar.
        plain = subprocess.run(render + ['--out', str(self.root / 'plain')],
                               capture_output=True, timeout=180)
        self.assertEqual(plain.returncode, 0, plain.stderr.decode())
        self.assertEqual(sorted(item.suffix for item in (self.root / 'plain').glob('*')),
                         ['.png'] * 3)
        for arguments, message in ((['--subtitles', 'srt,ass'], b'srt, vtt and ttml'),
                                   (['--subtitles', 'srt,srt'], b'at most once'),
                                   (['--subtitle-nodes', '3'], b'needs --subtitles'),
                                   (['--subtitles', 'srt', '--subtitle-nodes', 'x'], b'node ids'),
                                   (['--subtitles', 'srt', '--embed-subtitles'],
                                    b'SRT, WebVTT or TTML sidecar files')):
            refused = subprocess.run(render + ['--out', str(self.root / 'refused')] + arguments,
                                     capture_output=True, timeout=60)
            self.assertEqual(refused.returncode, 2, arguments)
            self.assertIn(message, refused.stderr, arguments)
        self.assertFalse((self.root / 'refused').exists())


class Cli(unittest.TestCase):
    """`bloom-cli run` takes the same operations and refuses the same limits."""

    def run_script(self, operations):
        with tempfile.TemporaryDirectory(prefix='bloom-subtitles-cli-') as directory:
            script = pathlib.Path(directory) / 'script.json'
            script.write_text(json.dumps(operations))
            return subprocess.run([CLI, 'run', str(script)], capture_output=True, timeout=120)

    def test_run_builds_and_writes_the_same_list(self):
        server = Server()
        try:
            server.tool('project.new', {'discardChanges': True, 'stopJobs': True})
            result = server.transact({'op': 'bloom.layer.add-subtitles', 'args': {
                'composition': 1, 'cues': [{'start': '00:00:01.000', 'end': '00:00:02.000',
                                            'text': 'One'}]}})
            node = outputs(result, 'subtitlesNode')[0]
        finally:
            server.close()
        with tempfile.TemporaryDirectory(prefix='bloom-subtitles-cli-out-') as directory:
            target = pathlib.Path(directory) / 'out.srt'
            run = self.run_script([
                {'op': 'bloom.layer.add-subtitles', 'args': {'composition': 1, 'text': SRT}},
                {'op': 'bloom.subtitles.shift-cues', 'args': {'composition': 1, 'node': node,
                                                              'by': '00:00:01.000'}},
                {'op': 'bloom.subtitles.write-file', 'args': {'composition': 1, 'node': node,
                                                              'path': str(target),
                                                              'format': 'srt'}}])
            self.assertEqual(run.returncode, 0, run.stderr.decode())
            self.assertEqual(target.read_text(),
                             SRT.replace('00:00:01,000 --> 00:00:02,000',
                                         '00:00:02,000 --> 00:00:03,000')
                                .replace('00:00:02,500 --> 00:00:03,000',
                                         '00:00:03,500 --> 00:00:04,000'))

    def test_run_refuses_a_limit_naming_it(self):
        run = self.run_script([{'op': 'bloom.layer.add-subtitles', 'args': {
            'composition': 1, 'cues': [{'start': 0, 'end': 1, 'text': 'x' * 4097}]}}])
        self.assertEqual(run.returncode, 4, run.stderr.decode())
        self.assertIn('(limit subtitleCueTextBytes 4096, actual 4097)', run.stderr.decode())


if __name__ == '__main__':
    unittest.main()
