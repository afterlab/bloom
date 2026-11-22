"""#531: the Image Switch through every agent surface. MCP (the stdio server) puts a switch between
a red Solid and its layer with a Checker on its second input, keys nothing, and sets the index in
one transact: index 1 renders the Solid, 2 the Checker, 7 clamps to the Checker and 3 with Wrap
wraps to the Solid; help and the node reference describe the node. Python
(bloom.ops.node.add / connect / set_parameter_value) and `bloom-cli run` build the same switch.

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


def outputs(result, name):
    return [output['id'] for output in result['outputs'] if output['name'] == name]


def set_value(node, role, value):
    if isinstance(value, bool):
        key = 'boolean'
    elif isinstance(value, int):
        key = 'integer'
    else:
        key = 'number'
    return {'op': 'bloom.node.set-parameter-value', 'args': {'node': node, 'role': role,
                                                              key: value}}


def connect(source, source_port, target, port):
    return {'op': 'bloom.node.connect', 'args': {'composition': 1, 'sourceNode': source,
                                                 'sourcePort': source_port, 'targetNode': target,
                                                 'port': port}}


def add_node(node_type):
    return {'op': 'bloom.node.add', 'args': {'composition': 1, 'nodeType': node_type}}


class Server:
    """One stdio server kept open, so each call can read the previous call's result."""

    def __init__(self):
        self.process = stdio_child.start([MCP])
        self.next_id = 1
        self.call('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                                 'clientInfo': {'name': 'Bloom image switch', 'version': '1'}})
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


def switched(server):
    """A red Solid layer whose content is an Image Switch over the Solid and a Checker."""
    added = server.transact({'op': 'bloom.layer.add-solid', 'args': {
        'composition': 1, 'name': 'Red', 'color': [1, 0, 0, 1]}})
    assert added['succeeded'], json.dumps(added)
    solid = outputs(added, 'solidNode')[0]
    layer_output = outputs(added, 'layerOutputNode')[0]
    made = server.transact(add_node('bloom.image-switch'), add_node('bloom.checker-source'))
    assert made['succeeded'], json.dumps(made)
    switch, checker = outputs(made, 'node')
    wired = server.transact(connect(solid, 'image', switch, 'input1'),
                            connect(checker, 'image', switch, 'input2'),
                            connect(switch, 'image', layer_output, 'image'))
    assert wired['succeeded'], json.dumps(wired)
    return switch


class Mcp(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = Server()

    @classmethod
    def tearDownClass(cls):
        cls.server.close()

    def setUp(self):
        self.server.tool('project.new', {'discardChanges': True, 'stopJobs': True})

    def test_the_index_chooses_the_input(self):
        switch = switched(self.server)
        with tempfile.TemporaryDirectory(prefix='bloom-image-switch-') as folder:
            first = self.server.render(folder, 'first')
            self.assertTrue(self.server.transact(set_value(switch, 'index', 2.0))['succeeded'])
            second = self.server.render(folder, 'second')
            self.assertTrue(self.server.transact(set_value(switch, 'index', 7.0))['succeeded'])
            clamped = self.server.render(folder, 'clamped')
            result = self.server.transact(set_value(switch, 'index', 3.0),
                                          set_value(switch, 'wrap', True))
            self.assertTrue(result['succeeded'], json.dumps(result))
            wrapped = self.server.render(folder, 'wrapped')
        self.assertNotEqual(first, second)
        self.assertEqual(second, clamped)
        self.assertEqual(first, wrapped)

    def test_help_and_the_node_reference_describe_it(self):
        nodes = self.server.call('resources/read', {'uri': 'bloom://reference/nodes'})
        reference = nodes['result']['contents'][0]['text']
        for node_type, ports in (('bloom.image-switch', ('`input1`', '`input8`')),
                                 ('bloom.channel-set-switch', ('`channels1`', '`channels8`'))):
            section = reference[reference.index('(`{}`)'.format(node_type)):]
            section = section[:section.index('\n### ')]
            for fragment in ports + ('`index`', '`wrap`', '`selected`'):
                self.assertIn(fragment, section, node_type + ' ' + fragment)
        text = json.dumps(self.server.tool('help', {'name': 'bloom.node.add'}))
        self.assertIn('nodeType', text)


class Cli(unittest.TestCase):
    """bloom-cli run: the same switch from a JSON script."""

    def test_run_builds_the_switch(self):
        server = Server()
        try:
            server.tool('project.new', {'discardChanges': True, 'stopJobs': True})
            switch = switched(server)
            records = server.tool('query', {'kind': 'nodes', 'composition': 1})['records']
        finally:
            server.close()
        by_type = {}
        for record in records:
            by_type.setdefault(record.get('type') or record.get('typeId'), []).append(record['id'])
        solid = by_type['bloom.solid-source'][0]
        checker = by_type['bloom.checker-source'][0]
        layer_output = by_type['bloom.layer-output'][0]
        script = [{'op': 'bloom.layer.add-solid', 'args': {'composition': 1, 'name': 'Red',
                                                           'color': [1, 0, 0, 1]}},
                  add_node('bloom.image-switch'), add_node('bloom.checker-source'),
                  connect(solid, 'image', switch, 'input1'),
                  connect(checker, 'image', switch, 'input2'),
                  connect(switch, 'image', layer_output, 'image'),
                  set_value(switch, 'index', 2.0), set_value(switch, 'wrap', True)]
        with tempfile.TemporaryDirectory(prefix='bloom-image-switch-cli-') as directory:
            path = pathlib.Path(directory) / 'script.json'
            path.write_text(json.dumps(script))
            run = subprocess.run([CLI, 'run', str(path)], capture_output=True, timeout=120)
            unknown = pathlib.Path(directory) / 'unknown.json'
            unknown.write_text(json.dumps(script[:2] + [set_value(switch, 'choice', 2.0)]))
            refused = subprocess.run([CLI, 'run', str(unknown)], capture_output=True,
                                     timeout=120)
        self.assertEqual(run.returncode, 0, run.stderr.decode())
        self.assertIn('executed {} transaction(s)'.format(len(script)), run.stdout.decode())
        self.assertNotEqual(refused.returncode, 0)
        self.assertIn('choice', refused.stderr.decode() + refused.stdout.decode())


@unittest.skipIf(PYTHON == '-', 'built without Python')
class Python(unittest.TestCase):
    """The same steps from Python."""

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

    def test_switch(self):
        added = bloom.ops.layer.add_solid(name='Red', color=[1, 0, 0, 1])
        self.assertTrue(added['succeeded'])
        solid = outputs(added, 'solidNode')[0]
        layer_output = outputs(added, 'layerOutputNode')[0]
        switch = outputs(bloom.ops.node.add(nodeType='bloom.image-switch'), 'node')[0]
        checker = outputs(bloom.ops.node.add(nodeType='bloom.checker-source'), 'node')[0]
        for source, source_port, target, port in ((solid, 'image', switch, 'input1'),
                                                  (checker, 'image', switch, 'input2'),
                                                  (switch, 'image', layer_output, 'image')):
            result = bloom.ops.node.connect(sourceNode=source, sourcePort=source_port,
                                            targetNode=target, port=port)
            self.assertTrue(result['succeeded'], port)
        result = bloom.ops.node.set_parameter_value(node=switch, role='index', number=2.0)
        self.assertTrue(result['succeeded'])
        composition = bloom.context.composition
        for node in bloom.data.nodes(composition):
            record = node.resolve()
            if record['type_id'] == 'bloom.image-switch':
                self.assertIn('wrap', json.dumps(record, default=str))


if __name__ == '__main__':
    unittest.main()
