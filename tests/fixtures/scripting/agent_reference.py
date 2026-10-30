"""The agent surfaces are self-describing and read back what they write (bloom.mcp.reference).

Usage: agent_reference.py <bloom-mcp> <bloom-cli> <python-dir or ->

MCP, bloom-cli and Python print the same reference entry; every tool, argument and enum is
described; the resources are readable; and every value written through transact reads back
through query in the units it was written in.
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

MCP, CLI, PYTHON_DIR = sys.argv[1:4]
del sys.argv[1:4]
if PYTHON_DIR != '-':
    sys.path.insert(0, PYTHON_DIR)
    import _bloom  # noqa: E402
else:
    _bloom = None

SAMPLES = ['bloom.layer.add-solid', 'bloom.animation.set-keyframe-at-time-for-parameter',
           'query', 'transact', 'capture', 'query:layers', 'query:keyframes', 'concept:time',
           'bloom-cli render', 'bloom.help']


def request(id, method, params=None):
    message = {'jsonrpc': '2.0', 'id': id, 'method': method}
    if params is not None:
        message['params'] = params
    return message


class Server:
    """One bloom-mcp process driven line by line."""

    def __init__(self):
        self.process = subprocess.Popen([MCP], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE)
        self.next = 1
        self.initialize = self.send('initialize', {
            'protocolVersion': '2025-11-25', 'capabilities': {},
            'clientInfo': {'name': 'reference test', 'version': '1'}})
        self.process.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
        self.process.stdin.flush()
        self.revision = 0

    def close(self):
        self.process.stdin.close()
        self.process.wait(timeout=30)
        self.process.stdout.close()
        self.process.stderr.close()

    def send(self, method, params=None):
        self.next += 1
        line = json.dumps(request(self.next, method, params)) + '\n'
        self.process.stdin.write(line.encode())
        self.process.stdin.flush()
        response = self.process.stdout.readline()
        if not response:
            raise AssertionError(self.process.stderr.read().decode(errors='replace'))
        return json.loads(response)

    def tool(self, name, arguments):
        response = self.send('tools/call', {'name': name, 'arguments': arguments})
        if 'error' in response:
            raise AssertionError(f'{name} {arguments}: {response["error"]}')
        return response['result']['structuredContent']

    def query(self, **arguments):
        result = self.tool('query', arguments)
        self.revision = result.get('revision', self.revision)
        return result

    def transact(self, *operations):
        result = self.tool('transact', {'expectedRevision': self.revision, 'operations': [
            {'op': op, 'args': args} for op, args in operations]})
        if not result.get('succeeded'):
            raise AssertionError(json.dumps(result))
        self.revision = result['revision']
        return {(item['operationIndex'], item['name']): item['id'] for item in result['outputs']}


def cli(*arguments):
    return subprocess.run([CLI, *arguments], capture_output=True, timeout=60)


def tiny_png(path):
    def chunk(kind, data):
        body = kind + data
        return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body))
    raw = b''.join(b'\x00' + b'\xff\x00\x00\xff' * 4 for _ in range(4))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 4, 4, 8, 6, 0,
                                                                        0, 0))
                     + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


class Reference(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = Server()

    @classmethod
    def tearDownClass(cls):
        cls.server.close()

    def test_initialize_advertises_instructions_and_resources(self):
        result = self.server.initialize['result']
        self.assertIn('resources', result['capabilities'])
        self.assertIn('help', result['instructions'])
        self.assertIn('bloom://guide/agents', result['instructions'])

    def test_tools_list_is_described_by_the_reference(self):
        tools = self.server.send('tools/list')['result']['tools']
        self.assertEqual(tools[0]['name'], 'help')
        for tool in tools:
            entry = self.server.tool('help', {'name': tool['name']})
            self.assertTrue(entry['found'], tool['name'])
            self.assertIn('help {"name":"' + tool['name'] + '"}', tool['description'])
            arguments = {argument['name']: argument for argument in entry['entry']['arguments']}
            for name, schema in tool['inputSchema']['properties'].items():
                where = f"{tool['name']}.{name}"
                self.assertIn(name, arguments, f'{where} is not in the reference')
                self.assertTrue(schema.get('description'), f'{where} has no description')
                self.assertTrue(arguments[name]['type'], where)
                if 'enum' in schema:
                    self.assertEqual(sorted(schema['enum']),
                                     sorted(choice['value'] for choice in
                                            arguments[name]['choices']), where)

    def test_every_operation_is_in_the_index(self):
        index = self.server.tool('help', {})
        names = {entry['name'] for entry in index['index']['entries']}
        for record in self.server.query(kind='operations')['records']:
            self.assertIn(record['id'], names)
            self.assertTrue(record['description'], record['id'])
            for argument in record['arguments']:
                self.assertTrue(argument['summary'], f"{record['id']}.{argument['name']}")
        for kind in ('query:layers', 'query:keyframes', 'query:node-types', 'bloom-cli help'):
            self.assertIn(kind, names)

    def test_every_surface_prints_the_same_entry(self):
        for name in SAMPLES:
            mcp = self.server.tool('help', {'name': name})
            self.assertTrue(mcp['found'], name)
            text = cli('help', name)
            self.assertEqual(text.returncode, 0, name)
            self.assertEqual(text.stdout.decode(), mcp['text'], name)
            data = cli('help', name, '--json')
            self.assertEqual(json.loads(data.stdout), mcp, name)
            if _bloom is not None:
                found, python = _bloom.reference(name, False)
                self.assertTrue(found)
                self.assertEqual(python, mcp['text'], name)
                self.assertEqual(json.loads(_bloom.reference(name, True)[1]), mcp, name)
        self.assertEqual(cli('help').stdout.decode(), self.server.tool('help', {})['text'])

    def test_unknown_names_suggest_the_closest_entry(self):
        missing = self.server.tool('help', {'name': 'bloom.layer.add-solidd'})
        self.assertFalse(missing['found'])
        self.assertIn('bloom.layer.add-solid', missing['suggestions'])
        self.assertEqual(cli('help', 'bloom.layer.add-solidd').returncode, 1)
        self.assertIn('bloom-cli help', cli('--help').stderr.decode())
        # A command name finds bloom-cli's own command, and <command> --help is the same entry.
        command = cli('help', 'render').stdout.decode()
        self.assertTrue(command.startswith('bloom-cli render'))
        self.assertEqual(cli('render', '--help').stdout.decode(), command)

    def test_resources_are_readable(self):
        listed = self.server.send('resources/list')['result']['resources']
        uris = {resource['uri'] for resource in listed}
        for uri in ('bloom://guide/agents', 'bloom://reference/nodes',
                    'bloom://reference/operations', 'bloom://reference/index.json',
                    'bloom://guide/mcp', 'bloom://guide/scripting', 'bloom://guide/python'):
            self.assertIn(uri, uris)
        for resource in listed:
            contents = self.server.send('resources/read', {'uri': resource['uri']})
            text = contents['result']['contents'][0]['text']
            self.assertGreater(len(text), 200, resource['uri'])
            self.assertEqual(contents['result']['contents'][0]['mimeType'], resource['mimeType'])
        guide = self.server.send('resources/read', {'uri': 'bloom://guide/mcp'})
        self.assertTrue(guide['result']['contents'][0]['text'].startswith('# Using Bloom'))
        nodes = self.server.send('resources/read', {'uri': 'bloom://reference/nodes'})
        self.assertIn('3 Screen', nodes['result']['contents'][0]['text'])
        self.assertIn('error', self.server.send('resources/read', {'uri': 'bloom://nothing'}))

    def test_the_agent_guide_runs_as_written(self):
        guide = self.server.send('resources/read', {'uri': 'bloom://guide/agents'})
        text = guide['result']['contents'][0]['text']
        blocks = [json.loads(block.split('```', 1)[0]) for block in text.split('```json\n')[1:]]
        fresh = Server()
        try:
            outputs = {}
            for call in blocks:
                operations = call['arguments'].get('operations', [])
                if any(op['op'] == 'bloom.composition.add' for op in operations):
                    continue  # Only for a project without a composition.
                result = fresh.tool(call['name'], call['arguments'])
                self.assertNotEqual(result.get('succeeded'), False, json.dumps(result))
                for item in result.get('outputs', []):
                    outputs[item['name']] = item['id']
                for op in operations:
                    if 'parameter' in op['args']:
                        self.assertEqual(op['args']['parameter'], outputs['positionParameter'],
                                         'the guide names a stale parameter id')
        finally:
            fresh.close()

    def test_query_refusals_name_the_way_out(self):
        response = self.server.send('tools/call', {'name': 'query',
                                                   'arguments': {'kind': 'nonsense'}})
        self.assertIn('keyframes', response['error']['message'])


class ReadBack(unittest.TestCase):
    """What transact writes, query returns, in the same units."""

    def setUp(self):
        self.server = Server()
        self.server.query(kind='context')
        self.composition = self.server.query(kind='compositions')['records'][0]['id']

    def tearDown(self):
        self.server.close()

    def composition_record(self):
        records = self.server.query(kind='compositions')['records']
        return next(record for record in records if record['id'] == self.composition)

    def test_composition_timing_round_trips(self):
        record = self.composition_record()
        for field in ('duration', 'start', 'workArea', 'frameRate', 'fps', 'pixelAspect',
                      'background', 'safeAreas', 'layerCount', 'dropFrame'):
            self.assertIn(field, record)
        rate = record['frameRate']
        self.server.transact(('bloom.composition.set-work-area', {
            'composition': self.composition, 'start': [1, 2], 'end': [3, 2]}))
        work = self.composition_record()['workArea']
        self.assertEqual(work['start']['seconds'], [1, 2])
        self.assertEqual(work['end']['seconds'], [3, 2])
        self.assertEqual(work['start']['frames'], rate[0] // (2 * rate[1]))
        # A read value can be written back as it was read.
        self.server.transact(('bloom.composition.set-work-area', {
            'composition': self.composition, 'start': work['start']['seconds'],
            'end': work['end']['seconds']}))
        self.assertEqual(self.composition_record()['workArea'], work)

    def test_layers_parameters_and_keyframes_round_trip(self):
        ids = self.server.transact(
            ('bloom.layer.add-solid', {'composition': self.composition, 'name': 'Red',
                                       'color': [1, 0, 0, 1]}),
            ('bloom.layer.add-text', {'composition': self.composition, 'name': 'Title',
                                      'text': 'Hello'}))
        layers = self.server.query(kind='layers', composition=self.composition)['records']
        self.assertEqual([layer['name'] for layer in layers[:2]], ['Title', 'Red'])
        self.assertEqual([layer['index'] for layer in layers[:2]], [0, 1])
        red = layers[1]
        self.assertEqual(red['id'], ids[(0, 'layer')])
        self.assertEqual(red['layerOutputNode'], ids[(0, 'layerOutputNode')])
        self.assertEqual(red['parameters']['position'], ids[(0, 'positionParameter')])
        self.assertEqual(red['sourceType'], 'bloom.solid-source')
        self.assertEqual(red['sourceParameters']['color'], ids[(0, 'colorParameter')])
        self.assertEqual(red['end'], self.composition_record()['duration'])
        self.assertIn('extent', red['timing'])

        # Enumerations: the metadata lists the labelled values; the value written reads back.
        blend = red['parameters']['blendMode']
        parameter = self.server.query(kind='parameter', composition=self.composition,
                                      parameter=blend)
        choices = {choice['label']: choice['value'] for choice in
                   parameter['metadata']['choices']}
        self.assertEqual(choices['Screen'], 3)
        self.assertEqual(parameter['metadata']['owner']['role'], 'blendMode')
        # Constant values read back as written.
        self.server.transact(('bloom.parameter.set-source', {
            'composition': self.composition, 'parameter': red['parameters']['opacity'],
            'value': 0.25}))
        record = self.server.query(kind='parameter', composition=self.composition,
                                   parameter=red['parameters']['opacity'])['record']
        self.assertEqual(record['value'], 0.25)
        opacity = self.server.query(kind='parameter', composition=self.composition,
                                    parameter=red['parameters']['opacity'])['metadata']
        self.assertEqual((opacity['minimum'], opacity['maximum']), (0, 1))
        self.assertEqual(opacity['valueKind'], 'scalar')
        self.assertTrue(opacity['animatable'])

        # Keyframes come back with their time in frames and seconds and their value as written.
        position = red['parameters']['position']
        self.server.transact(
            ('bloom.animation.create-for-parameter', {
                'composition': self.composition, 'parameter': position, 'time': [0, 1]}),
            ('bloom.animation.set-keyframe-at-time-for-parameter', {
                'composition': self.composition, 'parameter': position, 'time': [0, 1],
                'value': [100, 200]}),
            ('bloom.animation.set-keyframe-at-time-for-parameter', {
                'composition': self.composition, 'parameter': position, 'time': [1, 1],
                'value': [300, 400]}))
        keys = self.server.query(kind='keyframes', composition=self.composition,
                                 parameter=position)
        self.assertEqual(keys['valueKind'], 'vector2')
        self.assertEqual(keys['totalRecords'], 4)
        rate = keys['frameRate']
        by_time = {}
        for row in keys['records']:
            by_time.setdefault(tuple(row['time']['seconds']), {})[row['component']] = row['value']
            self.assertEqual(row['interpolation'], 'linear')
        self.assertEqual(by_time[(0, 1)], {'x': 100, 'y': 200})
        self.assertEqual(by_time[(1, 1)], {'x': 300, 'y': 400})
        frames = sorted({row['time']['frames'] for row in keys['records']})
        self.assertEqual(frames, [0, rate[0] // rate[1]])
        # Paging walks the same rows.
        rows, cursor = [], None
        while True:
            page = self.server.query(kind='keyframes', composition=self.composition,
                                     parameter=position, pageSize=1,
                                     **({'cursor': cursor} if cursor else {}))
            rows += page['records']
            cursor = page['nextCursor']
            if cursor is None:
                break
        self.assertEqual(rows, keys['records'])

    def test_node_graph_reads_back_links_ports_and_positions(self):
        ids = self.server.transact(('bloom.layer.add-solid', {
            'composition': self.composition, 'name': 'Red', 'color': [1, 0, 0, 1]}))
        graph = self.server.query(kind='nodes', composition=self.composition)
        nodes = {node['id']: node for node in graph['records']}
        layer_output = nodes[ids[(0, 'layerOutputNode')]]
        sources = [port['source'] for port in layer_output['inputs'] if port['source']]
        self.assertIn(ids[(0, 'solidNode')], [source['node'] for source in sources])
        self.assertEqual(len(layer_output['position']), 2)
        self.assertTrue(graph['links'])
        merge = next(node for node in graph['records'] if 'stack' in node)
        self.assertIn(ids[(0, 'layer')], [row['layer'] for row in merge['stack']])
        self.server.transact(('bloom.node.move', {
            'composition': self.composition,
            'positions': [[ids[(0, 'solidNode')], 512, 256]]}))
        moved = next(node for node in self.server.query(
            kind='nodes', composition=self.composition)['records']
            if node['id'] == ids[(0, 'solidNode')])
        self.assertEqual(moved['position'], [512, 256])

    def test_node_types_describe_ports_and_parameters(self):
        types = self.server.query(kind='node-types')['records']
        solid = next(record for record in types if record['id'] == 'bloom.solid-source')
        self.assertTrue(solid['outputs'])
        full = self.server.query(kind='node-types', nodeType='bloom.solid-source')['record']
        color = next(item for item in full['parameters'] if item['role'] == 'color')
        self.assertEqual(color['metadata']['valueKind'], 'color')

    def test_asset_kind_is_a_name(self):
        with tempfile.TemporaryDirectory(prefix='bloom-reference-') as directory:
            image = pathlib.Path(directory) / 'red.png'
            tiny_png(image)
            self.server.transact(('bloom.asset.import', {'paths': [str(image)],
                                                         'projectDirectory': directory}))
            assets = self.server.query(kind='assets')['records']
            self.assertEqual(assets[-1]['kind'], 'image')
            self.assertEqual(assets[-1]['tags'], [])


if __name__ == '__main__':
    unittest.main()
