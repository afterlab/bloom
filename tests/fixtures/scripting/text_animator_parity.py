"""#758: the Text Animator and fixed-width digits through every agent surface. MCP (the stdio
server) builds the three help recipes - a counter in fixed-width digits, a type-on with a cursor and
a word fade - and a Subtitles cue typing itself on through its progress output, each from the
operations help names, and renders them; Python (bloom.ops) builds the type-on and the counter, and
bloom-cli run builds the type-on from a JSON script.

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

ANIMATOR = 'bloom.text-animator'


def outputs(result, name):
    return [output['id'] for output in result['outputs'] if output['name'] == name]


class Server:
    """One stdio server kept open, so each call can read the previous call's result."""

    def __init__(self):
        self.process = stdio_child.start([MCP])
        self.next_id = 1
        self.call('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                                 'clientInfo': {'name': 'Bloom text animator', 'version': '1'}})
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

    def transact(self, *operations):
        revision = self.tool('query', {'kind': 'project'})['revision']
        result = self.tool('transact', {'expectedRevision': revision,
                                        'operations': list(operations)})
        if not result.get('succeeded'):
            raise AssertionError(json.dumps(result))
        self.last = result
        return result

    def value(self, parameter):
        return self.tool('query', {'kind': 'parameter', 'composition': 1,
                                   'parameter': parameter})['record']['value']

    def frame(self, frame):
        """The rendered frame's PNG data, after checking the capture succeeded."""
        capture = self.call('tools/call', {'name': 'capture',
                                           'arguments': {'composition': 1, 'frame': frame}})
        if 'error' in capture or capture['result'].get('isError', False):
            raise AssertionError(json.dumps(capture)[:400])
        images = [item['data'] for item in capture['result']['content'] if item['type'] == 'image']
        if len(images) != 1:
            raise AssertionError('the capture holds no image')
        return images[0]

    def close(self):
        stdio_child.end(self.process, 60)


def op(operation, /, **args):
    return {'op': operation, 'args': {'composition': 1, **args}}


class Mcp(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = Server()

    @classmethod
    def tearDownClass(cls):
        cls.server.close()

    def setUp(self):
        self.server.tool('project.new', {'discardChanges': True, 'stopJobs': True})

    def add_text(self, text, **args):
        return self.server.transact(op('bloom.layer.add-text', name='Text', text=text, size=96,
                                       position=[200, 540], **args))

    def insert_animator(self, added):
        """A Text Animator on the link into the text's Layer Output, as help says."""
        inserted = self.server.transact(op('bloom.node.insert-on-link', nodeType=ANIMATOR,
                                           node=outputs(added, 'layerOutputNode')[0],
                                           port='image', position=[200, 0]))
        node = outputs(inserted, 'node')[0]
        nodes = self.server.tool('query', {'kind': 'nodes', 'composition': 1})['records']
        record = next(entry for entry in nodes if entry['id'] == node)
        self.assertEqual(record['typeId'], ANIMATOR)
        return node, inserted

    def set_value(self, node, role, **value):
        self.server.transact(op('bloom.node.set-parameter-value', node=node, role=role, **value))

    def test_type_on(self):
        added = self.add_text('Typed on')
        before = self.server.frame(10)
        node, inserted = self.insert_animator(added)
        self.assertEqual(len(outputs(inserted, 'parameter.reveal')), 1)
        self.set_value(node, 'cursor', boolean=True)
        self.server.transact(op('bloom.animation.set-keys',
                                parameter=outputs(inserted, 'parameter.reveal')[0],
                                keys=[[0, 0], [10, 100]]))
        frames = [self.server.frame(frame) for frame in (0, 5, 10)]
        self.assertEqual(len(set(frames)), 3, 'the text types on frame by frame')
        self.assertNotEqual(frames[2], before, 'the cursor draws after the full text')

    def test_word_fade(self):
        added = self.add_text('one two three')
        node, inserted = self.insert_animator(added)
        self.set_value(node, 'selector1Units', integer=2)
        self.set_value(node, 'selector1Shape', integer=2)
        self.set_value(node, 'opacity', number=0.0)
        self.server.transact(op('bloom.animation.set-keys',
                                parameter=outputs(inserted, 'parameter.selector1Offset')[0],
                                keys=[[0, -100], [10, 100]]))
        frames = [self.server.frame(frame) for frame in (0, 5, 10)]
        self.assertEqual(len(set(frames)), 3, 'the words fade in turn')

    def test_counter(self):
        added = self.add_text('0', fixedDigits=True)
        self.assertEqual(self.server.value(outputs(added, 'fixedDigitsParameter')[0]), True)
        value = outputs(self.server.transact(op('bloom.node.add', nodeType='bloom.value-scalar')),
                        'node')
        text = outputs(self.server.transact(op('bloom.node.add',
                                               nodeType='bloom.scalar-to-string')), 'node')
        self.assertEqual((len(value), len(text)), (1, 1))
        records = self.server.tool('query', {'kind': 'nodes', 'composition': 1})['records']
        record = next(entry for entry in records if entry['id'] == value[0])
        self.server.transact(op('bloom.animation.set-keys', parameter=record['parameters']['value'],
                                keys=[[0, 0], [10, 1000]]))
        self.server.transact(
            op('bloom.node.connect', sourceNode=value[0], sourcePort='value', targetNode=text[0],
               port='value'),
            op('bloom.node.connect', sourceNode=text[0], sourcePort='result',
               targetNode=outputs(added, 'textNode')[0], port='text'))
        self.assertNotEqual(self.server.frame(2), self.server.frame(8), 'the counter counts')

    def test_subtitles_progress_drives_the_reveal(self):
        self.server.transact(op('bloom.layer.add-subtitles', cues=[
            {'start': [0, 1], 'end': [1, 1], 'text': 'Spoken'}]))
        subtitles = next(entry['id'] for entry in self.server.tool(
            'query', {'kind': 'nodes', 'composition': 1})['records']
            if entry['typeId'] == 'bloom.subtitles')
        added = self.add_text('unused')
        node, _ = self.insert_animator(added)
        mapped = outputs(self.server.transact(op('bloom.node.add', nodeType='bloom.map-range')),
                         'node')[0]
        self.set_value(mapped, 'toMax', number=100.0)
        self.server.transact(
            op('bloom.node.connect', sourceNode=subtitles, sourcePort='progress',
               targetNode=mapped, port='value'),
            op('bloom.node.connect', sourceNode=mapped, sourcePort='result', targetNode=node,
               port='reveal'),
            op('bloom.node.connect', sourceNode=subtitles, sourcePort='text',
               targetNode=outputs(added, 'textNode')[0], port='text'))
        self.assertNotEqual(self.server.frame(4), self.server.frame(16),
                            'the cue types on over its time')

    def test_help_names_the_recipes(self):
        text = json.dumps(self.server.tool('help', {'name': 'concept:text-animator'}))
        for fragment in ('bloom.node.insert-on-link', 'layerOutputNode', 'selector1Units',
                         'Ramp down', 'reveal', 'cursor', 'fixedDigits', 'scalar-to-string', 'progress', 'map-range', 'Intersect'):
            self.assertIn(fragment, text, fragment)
        self.assertIn('concept:text-animator',
                      json.dumps(self.server.tool('help', {'name': 'concept:text'})))
        self.assertIn('fixedDigits',
                      json.dumps(self.server.tool('help', {'name': 'bloom.layer.add-text'})))
        nodes = self.server.call('resources/read', {'uri': 'bloom://reference/nodes'})
        reference = nodes['result']['contents'][0]['text']
        section = reference[reference.index('(`bloom.text-animator`)'):]
        section = section[:section.index('\n### ')]
        for fragment in ('selector1', 'reveal', 'insert-on-link', 'concept:text-animator',
                         'selector4Seed', 'cursorBlink'):
            self.assertIn(fragment, section, fragment)


class Cli(unittest.TestCase):
    """bloom-cli run: the type-on from a JSON script, with the ids a new project gives."""

    def test_run_builds_the_type_on(self):
        with tempfile.TemporaryDirectory(prefix='bloom-text-animator-cli-') as directory:
            script = pathlib.Path(directory) / 'script.json'
            script.write_text(json.dumps([
                op('bloom.layer.add-text', name='Title', text='Typed', fixedDigits=True),
                op('bloom.node.insert-on-link', nodeType=ANIMATOR, node=4, port='image'),
                op('bloom.node.set-parameter-value', node=5, role='reveal', number=50.0),
                op('bloom.node.set-parameter-value', node=5, role='cursor', boolean=True)]))
            run = subprocess.run([CLI, 'run', str(script)], capture_output=True, timeout=120)
        self.assertEqual(run.returncode, 0, run.stderr.decode())
        self.assertIn('executed 4 transaction(s)', run.stdout.decode())

    def test_run_refuses_a_value_outside_its_domain(self):
        with tempfile.TemporaryDirectory(prefix='bloom-text-animator-cli-') as directory:
            script = pathlib.Path(directory) / 'script.json'
            script.write_text(json.dumps([
                op('bloom.layer.add-text', name='Title', text='Typed'),
                op('bloom.node.insert-on-link', nodeType=ANIMATOR, node=4, port='image'),
                op('bloom.node.set-parameter-value', node=5, role='opacity', number=2.0)]))
            run = subprocess.run([CLI, 'run', str(script)], capture_output=True, timeout=120)
        self.assertNotEqual(run.returncode, 0)


@unittest.skipIf(PYTHON == '-', 'built without Python')
class Python(unittest.TestCase):
    """The type-on and the counter from Python's bloom.ops."""

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

    def test_type_on_and_counter(self):
        added = bloom.ops.layer.add_text(name='Title', text='12.50', fixedDigits=True)
        self.assertTrue(added['succeeded'])
        self.assertEqual(len(outputs(added, 'fixedDigitsParameter')), 1)
        inserted = bloom.ops.node.insert_on_link(nodeType=ANIMATOR,
                                                 node=outputs(added, 'layerOutputNode')[0],
                                                 port='image')
        self.assertTrue(inserted['succeeded'])
        node = outputs(inserted, 'node')[0]
        self.assertTrue(bloom.ops.node.set_parameter_value(node=node, role='cursor',
                                                           boolean=True)['succeeded'])
        keyed = bloom.ops.animation.set_keys(parameter=outputs(inserted, 'parameter.reveal')[0],
                                             keys=[[0, 0], [10, 100]])
        self.assertTrue(keyed['succeeded'])
        composition = bloom.context.composition
        types = [entry.resolve()['type_id'] for entry in bloom.data.nodes(composition)]
        self.assertIn(ANIMATOR, types)


if __name__ == '__main__':
    unittest.main()
