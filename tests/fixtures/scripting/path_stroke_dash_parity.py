"""#759: Path Stroke dash patterns through every agent surface. MCP (the stdio server) strokes a
text's letters, sets a three-pair dash-dot pattern in one transact, reads it back and renders a
different picture than the solid stroke; help and the node reference describe the dash-dot recipe.
Python (bloom.ops.node.set_parameter_value) and `bloom-cli run` set the same pattern.

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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stdio_child  # noqa: E402

MCP = sys.argv.pop(1)
CLI = sys.argv.pop(1)
PYTHON = sys.argv.pop(1)

# A dash-dot-dash pattern with round caps: dash 12, gap 6, a dot, gap 6, dash 3, gap 6.
PATTERN = {'dashLength': 12.0, 'dashGap': 6.0, 'dash2': 0.0, 'gap2': 6.0, 'dash3': 3.0,
           'gap3': 6.0, 'dashOffset': 2.0}


def outputs(result, name):
    return [output['id'] for output in result['outputs'] if output['name'] == name]


def set_value(node, role, value):
    key = 'integer' if isinstance(value, int) and not isinstance(value, bool) else 'number'
    return {'op': 'bloom.node.set-parameter-value', 'args': {'node': node, 'role': role,
                                                              key: value}}


def pattern_operations(stroke):
    return [set_value(stroke, 'cap', 1), set_value(stroke, 'width', 3.0)] + [
        set_value(stroke, role, value) for role, value in PATTERN.items()]


class Server:
    """One stdio server kept open, so each call can read the previous call's result."""

    def __init__(self):
        self.process = stdio_child.start([MCP])
        self.next_id = 1
        self.call('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                                 'clientInfo': {'name': 'Bloom dash patterns', 'version': '1'}})
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

    def node(self, node_id):
        records = self.tool('query', {'kind': 'nodes', 'composition': 1})['records']
        return next(record for record in records if record['id'] == node_id)

    def value(self, parameter):
        return self.tool('query', {'kind': 'parameter', 'composition': 1,
                                   'parameter': parameter})['record']['value']

    def render(self, folder, name):
        destination = pathlib.Path(folder) / name
        destination.mkdir()
        result = self.tool('render', {'composition': 1, 'frame': 0, 'preset': 'PngRgba8SrgbV1',
                                      'destination': str(destination)})
        if 'files' not in result:
            raise AssertionError(json.dumps(result)[:400])
        return hashlib.sha256(pathlib.Path(result['files'][0]['path']).read_bytes()).hexdigest()

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

    def stroked_text(self):
        added = self.server.transact({'op': 'bloom.layer.add-text', 'args': {
            'composition': 1, 'name': 'Title', 'text': 'Dash', 'size': 200,
            'position': [400, 500]}})
        self.assertTrue(added['succeeded'], json.dumps(added))
        stroked = self.server.transact({'op': 'bloom.layer.stroke-outline', 'args': {
            'composition': 1, 'layer': outputs(added, 'layer')[0]}})
        self.assertTrue(stroked['succeeded'], json.dumps(stroked))
        return outputs(stroked, 'stroke')[0]

    def test_one_transact_sets_a_dash_dot_pattern(self):
        stroke = self.stroked_text()
        with tempfile.TemporaryDirectory(prefix='bloom-dash-patterns-') as folder:
            self.assertTrue(self.server.transact(set_value(stroke, 'width', 3.0),
                                                 set_value(stroke, 'cap', 1))['succeeded'])
            solid = self.server.render(folder, 'solid')
            self.assertTrue(self.server.transact(set_value(stroke, 'dashLength', 12.0),
                                                 set_value(stroke, 'dashGap', 6.0))['succeeded'])
            single = self.server.render(folder, 'single')
            result = self.server.transact(*pattern_operations(stroke))
            self.assertTrue(result['succeeded'], json.dumps(result))
            parameters = self.server.node(stroke)['parameters']
            for role, value in PATTERN.items():
                self.assertAlmostEqual(self.server.value(parameters[role]), value, places=9,
                                       msg=role)
            patterned = self.server.render(folder, 'patterned')
        self.assertNotEqual(solid, single)
        self.assertNotEqual(single, patterned)

    def test_a_negative_length_draws_solid(self):
        """Any finite length is a valid value; a negative one draws the stroke solid."""
        stroke = self.stroked_text()
        with tempfile.TemporaryDirectory(prefix='bloom-dash-patterns-') as folder:
            solid = self.server.render(folder, 'solid')
            result = self.server.transact(set_value(stroke, 'dashLength', 12.0),
                                          set_value(stroke, 'dash2', -1.0),
                                          set_value(stroke, 'gap2', 4.0))
            self.assertTrue(result['succeeded'], json.dumps(result))
            self.assertEqual(self.server.render(folder, 'negative'), solid)

    def test_help_describes_the_dash_dot_recipe(self):
        text = json.dumps(self.server.tool('help', {'name': 'concept:dash-patterns'}))
        for fragment in ('Dash-dot', 'Dotted', 'dash2', 'gap2', 'dash3', 'gap3', 'dashOffset',
                         'cap', 'Round', 'stroke-dasharray'):
            self.assertIn(fragment, text, fragment)
        nodes = self.server.call('resources/read', {'uri': 'bloom://reference/nodes'})
        reference = nodes['result']['contents'][0]['text']
        section = reference[reference.index('(`bloom.path-stroke`)'):]
        section = section[:section.index('\n### ')]
        for fragment in ('`dash2`', '`gap3`', 'Dash-dot', 'Dotted', 'concept:dash-patterns'):
            self.assertIn(fragment, section, fragment)


class Cli(unittest.TestCase):
    """bloom-cli run: the same pattern from a JSON script."""

    def test_run_sets_the_pattern(self):
        # A new project makes the same ids on every surface: find the Path Stroke that
        # stroke-outline makes, then set its pattern from a script.
        server = Server()
        try:
            server.tool('project.new', {'discardChanges': True, 'stopJobs': True})
            added = server.transact({'op': 'bloom.layer.add-text', 'args': {
                'composition': 1, 'name': 'Title', 'text': 'Dash'}})
            stroked = server.transact({'op': 'bloom.layer.stroke-outline', 'args': {
                'composition': 1, 'layer': outputs(added, 'layer')[0]}})
            stroke = outputs(stroked, 'stroke')[0]
        finally:
            server.close()
        build = [{'op': 'bloom.layer.add-text', 'args': {'composition': 1, 'name': 'Title',
                                                         'text': 'Dash'}},
                 {'op': 'bloom.layer.stroke-outline', 'args': {'composition': 1, 'layer': 1}}]
        with tempfile.TemporaryDirectory(prefix='bloom-dash-patterns-cli-') as directory:
            script = pathlib.Path(directory) / 'script.json'
            script.write_text(json.dumps(build + pattern_operations(stroke)))
            run = subprocess.run([CLI, 'run', str(script)], capture_output=True, timeout=120)
            unknown = pathlib.Path(directory) / 'unknown.json'
            unknown.write_text(json.dumps(build + [set_value(stroke, 'dash4', 1.0)]))
            refused = subprocess.run([CLI, 'run', str(unknown)], capture_output=True,
                                     timeout=120)
        self.assertEqual(run.returncode, 0, run.stderr.decode())
        self.assertIn('executed {} transaction(s)'.format(2 + len(pattern_operations(stroke))),
                      run.stdout.decode())
        self.assertNotEqual(refused.returncode, 0)
        self.assertIn('dash4', refused.stderr.decode() + refused.stdout.decode())


@unittest.skipIf(PYTHON == '-', 'built without Python')
class Python(unittest.TestCase):
    """The same steps from Python: bloom.ops.node.set_parameter_value per role."""

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

    def test_pattern(self):
        added = bloom.ops.layer.add_text(name='Title', text='Dash')
        self.assertTrue(added['succeeded'])
        stroked = bloom.ops.layer.stroke_outline(layer=outputs(added, 'layer')[0])
        self.assertTrue(stroked['succeeded'])
        stroke = outputs(stroked, 'stroke')[0]
        for role, value in PATTERN.items():
            result = bloom.ops.node.set_parameter_value(node=stroke, role=role, number=value)
            self.assertTrue(result['succeeded'], role)
        composition = bloom.context.composition
        for node in bloom.data.nodes(composition):
            record = node.resolve()
            if record['type_id'] == 'bloom.path-stroke':
                self.assertIn('dash2', json.dumps(record, default=str))


if __name__ == '__main__':
    unittest.main()
