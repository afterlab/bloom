"""#757: a styled Text Source and bloom.layer.stroke-outline through every agent surface. MCP (the
stdio server) builds an accent-word outlined caption in one transact and reads its styling back;
Python (bloom.ops.layer.add_text, bloom.ops.layer.stroke_outline) and `bloom-cli run` build the
same caption and stroke its letters, and every surface makes the same graph as the node menu's
Stroke Text (one commands::StrokeLayerOutline step: Text Outline -> Path Stroke -> Shape Source on a
new layer).

Arguments: bloom-mcp, bloom-cli, and the staged Python package folder (or "-" without Python).
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

CAPTION = 'Bloom <c.accent1>now</c> <b>live</b>'
STYLING = {'markup': True, 'accent1': [1, 0.55, 0, 1], 'outlineWidth': 6,
           'outlineColor': [0, 0, 0, 1], 'outlineJoin': 'miter', 'background': 'per-line',
           'backgroundColor': [0, 0, 0, 0.5], 'backgroundPadding': 0.5, 'backgroundRadius': 8}
# What each styling output reads back as (choices as their stored index).
EXPECTED = {'markupParameter': True, 'accent1Parameter': [1, 0.55, 0, 1],
            'outlineWidthParameter': 6, 'outlineColorParameter': [0, 0, 0, 1],
            'outlineJoinParameter': 1, 'backgroundParameter': 1,
            'backgroundColorParameter': [0, 0, 0, 0.5], 'backgroundPaddingParameter': 0.5,
            'backgroundRadiusParameter': 8}
STROKE_TYPES = ['bloom.text-outline', 'bloom.path-stroke', 'bloom.shape-source']


def outputs(result, name):
    return [output['id'] for output in result['outputs'] if output['name'] == name]


class Server:
    """One stdio server kept open, so each call can read the previous call's result."""

    def __init__(self):
        self.process = stdio_child.start([MCP])
        self.next_id = 1
        self.call('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                                 'clientInfo': {'name': 'Bloom text styling', 'version': '1'}})
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

    def nodes(self):
        return self.tool('query', {'kind': 'nodes', 'composition': 1})['records']

    def value(self, parameter):
        return self.tool('query', {'kind': 'parameter', 'composition': 1,
                                   'parameter': parameter})['record']['value']

    def close(self):
        stdio_child.end(self.process, 60)


def stroke_shape(nodes, before):
    """The type ids of the nodes a stroke added, in id order."""
    return [node['typeId'] for node in nodes if node['id'] not in before
            and node['typeId'] in STROKE_TYPES]


class Mcp(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = Server()

    @classmethod
    def tearDownClass(cls):
        cls.server.close()

    def setUp(self):
        self.server.tool('project.new', {'discardChanges': True, 'stopJobs': True})

    def test_one_transact_builds_an_accent_word_outlined_caption(self):
        result = self.server.transact({'op': 'bloom.layer.add-text', 'args': {
            'composition': 1, 'name': 'Caption', 'text': CAPTION, 'size': 96,
            'position': [960, 900], **STYLING}})
        self.assertTrue(result['succeeded'], json.dumps(result))
        for name, expected in EXPECTED.items():
            ids = outputs(result, name)
            self.assertEqual(len(ids), 1, name)
            value = self.server.value(ids[0])
            if isinstance(expected, list):
                self.assertEqual(len(value), 4, name)
                for got, want in zip(value, expected):
                    self.assertAlmostEqual(got, want, places=6, msg=name)
            else:
                self.assertEqual(value, expected, name)
        # The roles left out keep their defaults.
        self.assertAlmostEqual(self.server.value(outputs(result, 'accent2Parameter')[0])[2], 1.0)
        # The caption renders (a styled text, no compile failure).
        capture = self.server.call('tools/call', {'name': 'capture',
                                                  'arguments': {'composition': 1, 'frame': 0}})
        self.assertNotIn('error', capture)
        self.assertFalse(capture['result'].get('isError', False), json.dumps(capture)[:400])

    def test_add_text_refuses_an_unknown_choice(self):
        refused = self.server.transact({'op': 'bloom.layer.add-text', 'args': {
            'composition': 1, 'name': 'T', 'text': 'x', 'outlineJoin': 'square'}})
        self.assertFalse(refused['succeeded'])
        self.assertIn('outlineJoin', json.dumps(refused))

    def test_stroke_outline_makes_the_menu_graph(self):
        added = self.server.transact({'op': 'bloom.layer.add-text', 'args': {
            'composition': 1, 'name': 'Title', 'text': 'Stroke'}})
        layer = outputs(added, 'layer')[0]
        before = {node['id'] for node in self.server.nodes()}
        stroked = self.server.transact({'op': 'bloom.layer.stroke-outline', 'args': {
            'composition': 1, 'layer': layer}})
        self.assertTrue(stroked['succeeded'], json.dumps(stroked))
        for name in ('reader', 'stroke', 'shape', 'layer'):
            self.assertEqual(len(outputs(stroked, name)), 1, name)
        self.assertEqual(stroke_shape(self.server.nodes(), before), STROKE_TYPES)
        # A layer with no path mask cannot be stroked by its masks; the refusal changes nothing.
        refused = self.server.transact({'op': 'bloom.layer.stroke-outline', 'args': {
            'composition': 1, 'layer': layer, 'source': 'masks'}})
        self.assertFalse(refused['succeeded'])
        bad = self.server.transact({'op': 'bloom.layer.stroke-outline', 'args': {
            'composition': 1, 'layer': layer, 'source': 'glyphs'}})
        self.assertFalse(bad['succeeded'])
        self.assertIn('source', json.dumps(bad))

    def test_help_names_the_styling(self):
        entry = self.server.tool('help', {'name': 'concept:text'})
        text = json.dumps(entry)
        for fragment in ('accent1', 'outlineWidth', 'background', 'Alpha Contour',
                         'Width/2', 'Scalar To String', 'Substring', 'stroke-outline'):
            self.assertIn(fragment, text, fragment)
        add_text = json.dumps(self.server.tool('help', {'name': 'bloom.layer.add-text'}))
        for argument in ('markup', 'accent4', 'outlineJoin', 'backgroundRadius'):
            self.assertIn(argument, add_text, argument)
        nodes = self.server.call('resources/read', {'uri': 'bloom://reference/nodes'})
        reference = nodes['result']['contents'][0]['text']
        section = reference[reference.index('(`bloom.text-source`)'):]
        section = section[:section.index('\n### ')]
        for fragment in ('`markup`', '<c.accent1>', '`outlineJoin`', '`background`',
                         'Alpha Contour', 'Under, Offset = Width/2', 'Substring'):
            self.assertIn(fragment, section, fragment)


class Cli(unittest.TestCase):
    """bloom-cli run: the same two operations from a JSON script."""

    def test_run_builds_and_strokes_the_caption(self):
        with tempfile.TemporaryDirectory(prefix='bloom-text-styling-cli-') as directory:
            script = pathlib.Path(directory) / 'script.json'
            script.write_text(json.dumps([
                {'op': 'bloom.layer.add-text', 'args': {'composition': 1, 'name': 'Caption',
                                                        'text': CAPTION, **STYLING}},
                {'op': 'bloom.layer.stroke-outline', 'args': {'composition': 1, 'layer': 1}}]))
            run = subprocess.run([CLI, 'run', str(script)], capture_output=True, timeout=120)
        self.assertEqual(run.returncode, 0, run.stderr.decode())
        self.assertIn('executed 2 transaction(s)', run.stdout.decode())

    def test_run_refuses_an_unknown_choice(self):
        with tempfile.TemporaryDirectory(prefix='bloom-text-styling-cli-') as directory:
            script = pathlib.Path(directory) / 'script.json'
            script.write_text(json.dumps([
                {'op': 'bloom.layer.add-text', 'args': {'composition': 1, 'name': 'T',
                                                        'text': 'x', 'background': 'boxed'}}]))
            run = subprocess.run([CLI, 'run', str(script)], capture_output=True, timeout=120)
        self.assertNotEqual(run.returncode, 0)
        self.assertIn('background', run.stderr.decode())


@unittest.skipIf(PYTHON == '-', 'built without Python')
class Python(unittest.TestCase):
    """The same steps from Python: bloom.ops.layer.add_text and stroke_outline."""

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

    def tearDown(self):
        _state._host.reset(self.token)

    def test_caption_and_stroke(self):
        added = bloom.ops.layer.add_text(name='Caption', text=CAPTION, **STYLING)
        self.assertTrue(added['succeeded'])
        for name in EXPECTED:
            self.assertEqual(len(outputs(added, name)), 1, name)
        layer = outputs(added, 'layer')[0]
        stroked = bloom.ops.layer.stroke_outline(layer=layer)
        self.assertTrue(stroked['succeeded'])
        for name in ('reader', 'stroke', 'shape', 'layer'):
            self.assertEqual(len(outputs(stroked, name)), 1, name)
        composition = bloom.context.composition
        types = [node.resolve()['type_id'] for node in bloom.data.nodes(composition)]
        for type_id in STROKE_TYPES:
            self.assertIn(type_id, types, type_id)


if __name__ == '__main__':
    unittest.main()
