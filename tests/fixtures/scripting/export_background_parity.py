"""#760 and #761 through every agent surface. The `background` argument of MCP render, capture and
export, bloom.render's background= (Python) and bloom-cli render --background composite the frame
over the composition's Background Color: the PNG is opaque and shows the color where nothing covers
the frame, and the preservation report says so; a transparent request keeps alpha; a bad value is
refused. An exported movie's result lists the streams read back from the file (#761), and
export.presets states each preset's codecs.

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

# k/255 display values, so the 8-bit file holds the color exactly.
BACKGROUND = [204 / 255, 51 / 255, 26 / 255, 1.0]
EXPECTED = (204, 51, 26)
OPAQUE = 'opaque over composition background'


def png_pixels(path):
    """The RGBA8 pixels of a non-interlaced 8-bit RGBA PNG, as rows of (r, g, b, a)."""
    data = pathlib.Path(path).read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', path
    offset, idat, header = 8, b'', None
    while offset < len(data):
        length, kind = struct.unpack('>I4s', data[offset:offset + 8])
        body = data[offset + 8:offset + 8 + length]
        if kind == b'IHDR':
            header = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT':
            idat += body
        offset += 12 + length
    width, height, depth, color, _, _, interlace = header
    assert (depth, color, interlace) == (8, 6, 0), header
    raw = zlib.decompress(idat)
    stride = width * 4
    rows, previous = [], bytearray(stride)
    for y in range(height):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            left = line[x - 4] if x >= 4 else 0
            up = previous[x]
            corner = previous[x - 4] if x >= 4 else 0
            if kind == 1:
                line[x] = (line[x] + left) & 0xFF
            elif kind == 2:
                line[x] = (line[x] + up) & 0xFF
            elif kind == 3:
                line[x] = (line[x] + (left + up) // 2) & 0xFF
            elif kind == 4:
                estimate = left + up - corner
                pa, pb, pc = abs(estimate - left), abs(estimate - up), abs(estimate - corner)
                predictor = left if pa <= pb and pa <= pc else up if pb <= pc else corner
                line[x] = (line[x] + predictor) & 0xFF
        rows.append([tuple(line[x:x + 4]) for x in range(0, stride, 4)])
        previous = line
    return rows


def close(pixel, expected):
    return all(abs(a - b) <= 1 for a, b in zip(pixel[:3], expected))


class Server:
    def __init__(self):
        self.process = stdio_child.start([MCP])
        self.next_id = 1
        self.call('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                                 'clientInfo': {'name': 'Bloom background parity', 'version': '1'}})
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


class Mcp(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = Server()

    @classmethod
    def tearDownClass(cls):
        cls.server.close()

    def setUp(self):
        self.server.tool('project.new', {'discardChanges': True, 'stopJobs': True})
        changed = self.server.transact({'op': 'bloom.composition.set-background-color',
                                        'args': {'composition': 1, 'color': BACKGROUND}})
        self.assertTrue(changed['succeeded'], json.dumps(changed))
        self.directory = tempfile.TemporaryDirectory(prefix='bloom-background-parity-')
        self.root = pathlib.Path(self.directory.name)

    def tearDown(self):
        self.directory.cleanup()

    def render(self, name, **extra):
        return self.server.tool('render', {
            'composition': 1, 'frame': 0, 'preset': 'PngRgba8SrgbV1',
            'destination': str(self.root / name), 'overwrite': 'replace', **extra})

    def test_render_composites_over_the_background(self):
        backed = self.render('backed.png', background='composition')
        self.assertNotIn('error', backed, json.dumps(backed))
        self.assertIn(OPAQUE, backed['preservationReport'])
        pixels = png_pixels(self.root / 'backed.png')
        for row in pixels:
            for pixel in row:
                self.assertEqual(pixel[3], 255)
                self.assertTrue(close(pixel, EXPECTED), pixel)
        clear = self.render('clear.png', background='transparent')
        self.assertNotIn('error', clear, json.dumps(clear))
        self.assertNotIn(OPAQUE, clear['preservationReport'])
        self.assertEqual({pixel[3] for row in png_pixels(self.root / 'clear.png') for pixel in row},
                         {0})
        default = self.render('default.png')
        self.assertNotIn(OPAQUE, default['preservationReport'])

    def test_bad_background_is_refused(self):
        refused = self.render('bad.png', background='black')
        self.assertFalse(refused.get('succeeded', True), json.dumps(refused))
        self.assertIn('transparent or composition', json.dumps(refused))
        self.assertFalse((self.root / 'bad.png').exists())
        captured = self.server.tool('capture', {'composition': 1, 'frame': 0,
                                                'background': 'opaque'})
        self.assertFalse(captured.get('succeeded', True), json.dumps(captured))

    def test_capture_takes_the_background(self):
        captured = self.server.call('tools/call', {'name': 'capture', 'arguments': {
            'composition': 1, 'frame': 0, 'background': 'composition'}})
        self.assertNotIn('error', captured, json.dumps(captured)[:2000])
        self.assertFalse(captured['result'].get('isError', False))

    def test_export_lists_the_written_streams(self):
        exported = self.server.tool('export', {
            'composition': 1, 'first': 0, 'last': 11, 'preset': 'builtin:prores-4444-mov',
            'destination': str(self.root / 'movie.mov'), 'overwrite': 'replace',
            'background': 'composition'})
        self.assertNotIn('error', exported, json.dumps(exported))
        streams = exported['streams']
        self.assertEqual([stream['kind'] for stream in streams], ['video', 'audio'])
        video, audio = streams
        self.assertEqual((video['codec'], video['profile']), ('prores', '4444'))
        self.assertGreater(video['bitrate'], 0)
        self.assertEqual(audio['codec'], 'pcm_s16le')
        self.assertEqual(audio['channels'], 2)

    def test_presets_state_their_codecs(self):
        presets = self.server.tool('export.presets', {})
        listed = presets if isinstance(presets, list) else (
            presets.get('presets') or presets.get('result') or presets.get('value'))
        self.assertIsInstance(listed, list, json.dumps(presets)[:500])
        rows = {row['id']: row for row in listed}
        mp4 = rows['builtin:h264-mp4']
        self.assertEqual((mp4['container'], mp4['videoCodec'], mp4['audioCodec'],
                          mp4['audioBitrateKbps']), ('mp4', 'h264', 'aac', 192))
        webm = rows['builtin:vp9-webm']
        self.assertEqual((webm['audioCodec'], webm['audioSampleRate']), ('opus', 48000))
        reference = self.server.tool('help', {'name': 'export'})
        self.assertIn('AAC-LC', json.dumps(reference))


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
        bloom.ops.composition.set_background_color(tuple(BACKGROUND))
        self.directory = tempfile.TemporaryDirectory(prefix='bloom-background-python-')
        self.root = pathlib.Path(self.directory.name)

    def tearDown(self):
        _state._host.reset(self.token)
        self.directory.cleanup()

    def test_render_takes_the_background(self):
        result = bloom.render.frame(0, out=self.root / 'backed.png', background='composition')
        self.assertIn(OPAQUE, result.preservation_report)
        self.assertEqual(result.streams, ())
        pixel = png_pixels(self.root / 'backed.png')[0][0]
        self.assertEqual(pixel[3], 255)
        self.assertTrue(close(pixel, EXPECTED), pixel)
        plain = bloom.render.frame(0, out=self.root / 'plain.png')
        self.assertNotIn(OPAQUE, plain.preservation_report)
        self.assertEqual(png_pixels(self.root / 'plain.png')[0][0][3], 0)
        with self.assertRaises(ValueError):
            bloom.render.frame(0, out=self.root / 'bad.png', background='opaque')
        self.assertFalse((self.root / 'bad.png').exists())

    def test_cli_render_background_option(self):
        project = self.root / 'scene.bloom'
        self.host.save(str(project))
        render = [CLI, 'render', '--project', str(project), '--frame', '0', '--preset',
                  'PngRgba8SrgbV1']
        command = subprocess.run(render + ['--out', str(self.root / 'backed'),
                                           '--background', 'composition'],
                                 capture_output=True, timeout=180)
        self.assertEqual(command.returncode, 0, command.stderr.decode())
        self.assertIn(OPAQUE, command.stdout.decode())
        pixel = png_pixels(self.root / 'backed' / 'frame.png')[0][0]
        self.assertEqual(pixel[3], 255)
        self.assertTrue(close(pixel, EXPECTED), pixel)
        refused = subprocess.run(render + ['--out', str(self.root / 'refused'),
                                           '--background', 'black'],
                                 capture_output=True, timeout=60)
        self.assertEqual(refused.returncode, 2)
        self.assertIn(b'transparent or composition', refused.stderr)


if __name__ == '__main__':
    unittest.main()
