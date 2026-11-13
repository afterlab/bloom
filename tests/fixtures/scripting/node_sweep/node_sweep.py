#!/usr/bin/env python3
"""Renders every registered node type on the GPU and on the CPU reference and checks the result.

The node types come from the registry at run time (bloom-mcp, query kind=node-types), so a node
added later is swept without editing this file. For each type with an image output:

  1. A graph is built around it. The default graph feeds the node from a Four-Color Gradient and
     wires it to a layer's output; a type the default cannot build declares its own graph, or the
     reason it has none, in node_graphs.json. A type with an image output that has neither a
     working default graph nor a declaration fails the sweep.
  2. One frame is rendered twice: with the GPU device, and with every Vulkan driver hidden so the
     CPU reference renders it.
  3. A node on the default graph whose GPU frame is the gradient itself fails, unless
     node_graphs.json declares under identityAtDefaults why its defaults are the identity (#676):
     a node that silently does nothing is caught even though both routes agree.
  4. The two frames are compared: identical files, or every finite component within the
     repository's documented GPU gate (2e-6 absolute or relative to the CPU value,
     docs/architecture/gpu-backend.md) and every non-finite component equal.
  5. The route and both digests are compared with the committed baseline for this platform.

The sweep fails when a node moves off the GPU, loses parity, changes a digest without a baseline
update, has no baseline row, or when the baseline names a node the registry no longer has.

  node_sweep.py --mcp <bloom-mcp> --baseline <file> [--require-device] [--update-baseline]
                [--records <file.jsonl>] [--only type,type]
  node_sweep.py --compare <baseline> <baseline> [--cross-platform <file>]
  node_sweep.py --self-test

Exit status: 0 passed, 1 failed, 77 not run (no GPU device and --require-device not given).
"""

import argparse
import contextlib
import hashlib
import itertools
import json
import os
import queue
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

SOURCE = 'bloom.four-color-gradient'
FRAME = 3
WIDTH, HEIGHT = 640, 360
PRESET = 'FlatExrRgba32fLinRec709SceneV1'
GATE = 2e-6
NOT_RUN = 77
HERE = os.path.dirname(os.path.abspath(__file__))


class SweepError(Exception):
    """A node could not be built or rendered; the text is the reason recorded for it."""


# --------------------------------------------------------------------------------------------
# bloom-mcp over stdio


class Session:
    def __init__(self, mcp, env, cwd):
        # stderr goes to a file, never a pipe nobody reads: when the server dies, the failure says
        # how it exited and what it last wrote.
        self.errors = tempfile.TemporaryFile()
        self.process = subprocess.Popen([mcp], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=self.errors, env=env, cwd=cwd)
        # A thread reads the answers so a wait can time out on every platform.
        self.lines = queue.Queue()
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()
        self.sequence = 0
        self.rpc('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                                'clientInfo': {'name': 'node-sweep', 'version': '1'}})
        self.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})

    def read(self):
        for line in self.process.stdout:
            self.lines.put(line)
        self.lines.put(None)

    def send(self, message):
        try:
            self.process.stdin.write((json.dumps(message) + '\n').encode())
            self.process.stdin.flush()
        except OSError as error:
            raise SweepError(f'bloom-mcp is gone ({error}); {self.death()}') from error

    def death(self):
        """How a server that stopped answering exited, and the end of what it wrote to stderr."""
        try:
            status = self.process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            status = 'still running'
        try:
            self.errors.seek(0)
            tail = self.errors.read()[-1500:].decode(errors='replace').strip()
        except (OSError, ValueError):
            tail = ''
        return f'exit status {status}; stderr: {tail or "(empty)"}'

    def rpc(self, method, params, timeout=600):
        self.sequence += 1
        self.send({'jsonrpc': '2.0', 'id': self.sequence, 'method': method, 'params': params})
        end = time.monotonic() + timeout
        while True:
            try:
                line = self.lines.get(timeout=max(0.0, end - time.monotonic()))
            except queue.Empty:
                raise SweepError(f'{method} did not answer within {timeout} seconds') from None
            if line is None:
                raise SweepError(f'bloom-mcp exited; {self.death()}')
            message = json.loads(line)
            if message.get('id') == self.sequence:
                return message

    def tool(self, name, arguments):
        message = self.rpc('tools/call', {'name': name, 'arguments': arguments})
        if 'error' in message:
            raise SweepError(f'{name}: {json.dumps(message["error"])[:600]}')
        result = message['result']
        content = result.get('structuredContent')
        if result.get('isError'):
            text = ' '.join(item.get('text', '') for item in result.get('content', []))
            raise SweepError(f'{name}: {(json.dumps(content) if content else text)[:900]}')
        return content if content is not None else {}

    def transact(self, operation, arguments):
        revision = self.tool('query', {'kind': 'compositions'}).get('revision')
        answer = self.tool('transact', {'expectedRevision': revision,
                                        'operations': [{'op': operation, 'args': arguments}]})
        if not answer.get('succeeded'):
            raise SweepError(f'{operation}: {json.dumps(answer.get("diagnostics"))[:900]}')
        return {output['name']: output['id'] for output in answer.get('outputs', [])}

    def close(self):
        try:
            self.process.stdin.close()
            self.process.wait(timeout=30)
        except Exception:  # pylint: disable=broad-except
            self.process.kill()
            self.process.wait()
        self.errors.close()


# --------------------------------------------------------------------------------------------
# Graphs. Each builder returns the composition to render.


def image_inputs(node_type):
    return [port for port in node_type['inputs'] if port['kind'] == 'image']


def needs_declaration(node_type):
    """A required input that is not an image is something the default graph cannot feed."""
    return [port['port'] for port in node_type['inputs']
            if port.get('required') and port['kind'] != 'image']


def new_composition(session, name='Sweep'):
    composition = session.transact('bloom.composition.add',
                                   {'name': name, 'width': WIDTH, 'height': HEIGHT})['composition']
    layer = session.transact('bloom.layer.add-solid', {'composition': composition, 'name': 'In',
                                                       'color': [0.8, 0.4, 0.2, 1]})
    return composition, layer['layerOutputNode']


def gradient_composition(session, name='Sweep'):
    composition, layer_output = new_composition(session, name)
    source = session.transact('bloom.node.add-feeding',
                              {'composition': composition, 'nodeType': SOURCE,
                               'node': layer_output, 'port': 'image'})['node']
    return composition, layer_output, source


def build_chain(session, node_type, _work):
    """Gradient -> node -> layer output; further required image inputs are fed by the gradient."""
    composition, layer_output, source = gradient_composition(session)
    if node_type['id'] == SOURCE:
        return composition
    node = session.transact('bloom.node.add-feeding',
                            {'composition': composition, 'nodeType': node_type['id'],
                             'node': layer_output, 'port': 'image'})['node']
    for port in image_inputs(node_type)[1:]:
        if port.get('required'):
            session.transact('bloom.node.connect',
                             {'composition': composition, 'sourceNode': source,
                              'sourcePort': 'image', 'targetNode': node,
                              'port': port['port']})
    return composition


def build_composition_source(session, _node_type, _work):
    """A Composition Source of a gradient composition, wired to the layer output."""
    inner, _, _ = gradient_composition(session, 'Inner')
    composition, layer_output = new_composition(session)
    node = session.transact('bloom.node.add-composition-source',
                            {'composition': composition, 'source': inner})['node']
    session.transact('bloom.node.connect',
                     {'composition': composition, 'sourceNode': node, 'sourcePort': 'image',
                      'targetNode': layer_output, 'port': 'image'})
    return composition


def build_channels(session, node_type, _work):
    """The node's channel-set input is fed by a Composition Source of a gradient composition."""
    inner, _, _ = gradient_composition(session, 'Inner')
    composition, layer_output = new_composition(session)
    channels = session.transact('bloom.node.add-composition-source',
                                {'composition': composition, 'source': inner})['node']
    node = session.transact('bloom.node.add-feeding',
                            {'composition': composition, 'nodeType': node_type['id'],
                             'node': layer_output, 'port': 'image'})['node']
    session.transact('bloom.node.connect',
                     {'composition': composition, 'sourceNode': channels,
                      'sourcePort': 'channels', 'targetNode': node, 'port': 'channels'})
    return composition


def write_png(path):
    """A small RGBA gradient as a PNG whose bytes are the same wherever this runs."""
    width, height = 64, 36
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            rows += bytes((x * 255 // (width - 1), y * 255 // (height - 1),
                           (x + y) * 255 // (width + height - 2), 255))

    def chunk(kind, data):
        body = kind + data
        return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body))

    # The rows are stored, not compressed, in deflate blocks written here, so no zlib version or
    # setting changes the file.
    stored = bytearray(b'\x78\x01')
    for start in range(0, len(rows), 65535):
        piece = bytes(rows[start:start + 65535])
        last = start + 65535 >= len(rows)
        stored += struct.pack('<BHH', 1 if last else 0, len(piece), len(piece) ^ 0xFFFF) + piece
    stored += struct.pack('>I', zlib.adler32(bytes(rows)))
    header = struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)
    data = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) + chunk(b'IDAT', bytes(stored)) +
            chunk(b'IEND', b''))
    with open(path, 'wb') as file:
        file.write(data)


def build_asset(session, path):
    """A source node of an imported file, wired to the layer output."""
    composition, layer_output = new_composition(session)
    before = {asset['id'] for asset in session.tool('query', {'kind': 'assets'}).get('records', [])}
    session.transact('bloom.asset.import', {'paths': [path]})
    after = [asset['id'] for asset in session.tool('query', {'kind': 'assets'}).get('records', [])
             if asset['id'] not in before]
    if len(after) != 1:
        raise SweepError(f'importing {os.path.basename(path)} added {len(after)} assets')
    node = session.transact('bloom.node.add-asset-source',
                            {'composition': composition, 'asset': after[0]})['node']
    session.transact('bloom.node.connect',
                     {'composition': composition, 'sourceNode': node, 'sourcePort': 'image',
                      'targetNode': layer_output, 'port': 'image'})
    return composition


def build_image_asset(session, _node_type, work):
    """An Image Source of a generated PNG."""
    path = os.path.join(work, 'sweep-source.png')
    write_png(path)
    return build_asset(session, path)


def build_video_asset(session, _node_type, _work):
    """A Video Source of the repository's committed test movie."""
    return build_asset(session, os.path.join(HERE, '..', '..', '..', '..', 'apps', 'bloom-media-worker',
                                             'tests', 'data', 'av1-flat.mp4'))


BUILDERS = {
    'chain': build_chain,
    'composition-source': build_composition_source,
    'channels': build_channels,
    'image-asset': build_image_asset,
    'video-asset': build_video_asset,
}


def plan(node_types, declarations):
    """What to do with every type that has an image output: (builder name | None, reason)."""
    planned = {}
    for node_type in node_types:
        if not any(output['kind'] == 'image' for output in node_type['outputs']):
            continue
        declared = declarations.get(node_type['id'])
        if declared and 'excluded' in declared:
            planned[node_type['id']] = (None, declared['excluded'])
        elif declared:
            if declared.get('graph') not in BUILDERS:
                raise SweepError(f'{node_type["id"]}: node_graphs.json names no known graph')
            planned[node_type['id']] = (declared['graph'], '')
        elif needs_declaration(node_type):
            raise SweepError(
                f'{node_type["id"]} has an image output and a required input the default graph '
                f'cannot feed ({", ".join(needs_declaration(node_type))}), and node_graphs.json '
                'declares no graph for it')
        else:
            planned[node_type['id']] = ('chain', '')
    stale = sorted(set(declarations) - {node_type['id'] for node_type in node_types})
    if stale:
        raise SweepError('node_graphs.json names types the registry does not have: ' +
                         ', '.join(stale))
    return planned


# --------------------------------------------------------------------------------------------
# Comparing two frames


def read_exr(path):
    """The float channels of a scanline OpenEXR file (no compression, ZIPS or ZIP)."""
    with open(path, 'rb') as file:
        data = file.read()
    if struct.unpack_from('<I', data, 0)[0] != 20000630:
        raise SweepError(f'{path} is not an OpenEXR file')
    if struct.unpack_from('<I', data, 4)[0] & 0x1E00:
        raise SweepError('the sweep reads single-part scanline OpenEXR files only')
    at = 8
    attributes = {}
    while data[at] != 0:
        end = data.index(b'\0', at)
        name = data[at:end].decode()
        at = end + 1
        end = data.index(b'\0', at)
        at = end + 1
        size = struct.unpack_from('<i', data, at)[0]
        attributes[name] = data[at + 4:at + 4 + size]
        at += 4 + size
    at += 1
    channels = []
    block = attributes['channels']
    cursor = 0
    while block[cursor] != 0:
        end = block.index(b'\0', cursor)
        pixel_type = struct.unpack_from('<i', block, end + 1)[0]
        if pixel_type != 2:
            raise SweepError('the sweep compares 32-bit float channels only')
        channels.append(block[cursor:end].decode())
        cursor = end + 1 + 16
    compression = attributes['compression'][0]
    lines_per_block = {0: 1, 2: 1, 3: 16}.get(compression)
    if lines_per_block is None:
        raise SweepError(f'the sweep does not read OpenEXR compression {compression}')
    x_min, y_min, x_max, y_max = struct.unpack('<4i', attributes['dataWindow'])
    width, height = x_max - x_min + 1, y_max - y_min + 1
    blocks = (height + lines_per_block - 1) // lines_per_block
    offsets = struct.unpack_from(f'<{blocks}Q', data, at)
    line_bytes = width * 4 * len(channels)
    planes = {name: [None] * height for name in channels}
    for offset in offsets:
        y, size = struct.unpack_from('<ii', data, offset)
        lines = min(lines_per_block, y_min + height - y)
        packed = data[offset + 8:offset + 8 + size]
        expected = lines * line_bytes
        if compression != 0 and size < expected:
            raw = zlib.decompress(packed)
            delta = bytes(itertools.accumulate(raw, lambda a, b: (a + b - 128) & 0xFF))
            half = (len(delta) + 1) // 2
            pixels = bytearray(len(delta))
            pixels[0::2] = delta[:half]
            pixels[1::2] = delta[half:]
        else:
            pixels = packed
        if len(pixels) != expected:
            raise SweepError(f'{path}: a scanline block holds {len(pixels)} bytes, not {expected}')
        for line in range(lines):
            base = line * line_bytes
            for index, name in enumerate(channels):
                start = base + index * width * 4
                planes[name][y - y_min + line] = pixels[start:start + width * 4]
    return width, height, planes


def compare_frames(gpu_path, cpu_path):
    """('identical' | 'within-gate' | 'differs', the largest difference beyond bit equality)."""
    with open(gpu_path, 'rb') as gpu, open(cpu_path, 'rb') as cpu:
        if gpu.read() == cpu.read():
            return 'identical', 0.0
    gpu_width, gpu_height, gpu_planes = read_exr(gpu_path)
    cpu_width, cpu_height, cpu_planes = read_exr(cpu_path)
    if (gpu_width, gpu_height) != (cpu_width, cpu_height) or set(gpu_planes) != set(cpu_planes):
        return 'differs', float('inf')
    largest = 0.0
    within = True
    for name, cpu_rows in cpu_planes.items():
        for gpu_row, cpu_row in zip(gpu_planes[name], cpu_rows):
            if gpu_row == cpu_row:
                continue
            count = len(cpu_row) // 4
            actual = struct.unpack(f'<{count}f', gpu_row)
            expected = struct.unpack(f'<{count}f', cpu_row)
            for a, e in zip(actual, expected):
                if a == e:
                    continue
                finite = abs(a) != float('inf') and abs(e) != float('inf') and a == a and e == e
                if not finite:
                    # A non-finite component has to be the same non-finite value.
                    if not (a != a and e != e):
                        return 'differs', float('inf')
                    continue
                difference = abs(a - e)
                largest = max(largest, difference)
                if difference > max(GATE, GATE * abs(e)):
                    within = False
    return ('within-gate' if within else 'differs'), largest


# --------------------------------------------------------------------------------------------
# The sweep


def environments(work):
    gpu = dict(os.environ, XDG_CACHE_HOME=os.path.join(work, 'cache-gpu'))
    hidden = os.path.join(work, 'no-vulkan-driver.json')
    cpu = dict(os.environ, XDG_CACHE_HOME=os.path.join(work, 'cache-cpu'),
               VK_ICD_FILENAMES=hidden, VK_DRIVER_FILES=hidden)
    return gpu, cpu


class LongSession:
    """One long-lived bloom-mcp for a route (GPU, or the driver hidden); restarted only after an error.

    Every use starts from a fresh untitled project, so nothing a node built can reach the next."""

    def __init__(self, mcp, env, work):
        self.mcp, self.env, self.work = mcp, env, work
        self.session = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.restart()

    def fresh(self):
        if self.session is None:
            self.session = Session(self.mcp, self.env, self.work)
        self.session.tool('project.new', {'discardChanges': True, 'stopJobs': True})
        return self.session

    def restart(self):
        """Drop a session that errored or hung; the next use starts a new process."""
        if self.session is not None:
            self.session.close()
            self.session = None


def render(route, work, node_type, builder, tag):
    try:
        session = route.fresh()
        composition = BUILDERS[builder](session, node_type, work)
        destination = os.path.join(work, f'{node_type["id"]}.{tag}.exr')
        answer = session.tool('render', {'composition': composition, 'frame': FRAME,
                                         'preset': PRESET, 'destination': destination,
                                         'overwrite': 'replace'})
        return {'path': destination, 'digest': answer['files'][0]['digest'],
                'provenance': answer.get('provenance'), 'routeNote': answer.get('routeNote')}
    except BaseException:
        route.restart()
        raise


def sweep(arguments):
    with open(os.path.join(HERE, 'node_graphs.json'), encoding='utf-8') as file:
        graphs = json.load(file)
    declarations = graphs['nodes']
    # #676: a node whose default frame is its input unchanged must say why its defaults are the
    # identity; any other node that passes the gradient through untouched silently does nothing.
    identity_at_defaults = graphs.get('identityAtDefaults', {})
    baseline = {'nodes': {}}
    if os.path.exists(arguments.baseline):
        with open(arguments.baseline, encoding='utf-8') as file:
            baseline = json.load(file)
    failures = []
    records = []
    with tempfile.TemporaryDirectory(prefix='bloom-node-sweep-') as work, \
            contextlib.ExitStack() as routes:
        gpu_env, cpu_env = environments(work)
        # Two long-lived sessions, one per route; each node gets a fresh project inside them.
        gpu_route = routes.enter_context(LongSession(arguments.mcp, gpu_env, work))
        cpu_route = routes.enter_context(LongSession(arguments.mcp, cpu_env, work))
        try:
            node_types = gpu_route.fresh().tool('query', {'kind': 'node-types'})['records']
        except SweepError:
            gpu_route.restart()
            raise
        by_id = {node_type['id']: node_type for node_type in node_types}
        planned = plan(node_types, declarations)

        # No device at all is "not run", never a pass.
        probe = render(gpu_route, work, by_id[SOURCE], 'chain', 'probe')
        if probe['provenance'] != 'gpu':
            message = f'no GPU device rendered the probe frame ({probe.get("routeNote")})'
            if arguments.require_device:
                print(f'FAIL: {message}')
                return 1
            print(f'NOT RUN: {message}')
            return NOT_RUN

        selected = set(arguments.only.split(',')) if arguments.only else None
        for type_id in sorted(planned):
            if selected and type_id not in selected:
                continue
            builder, reason = planned[type_id]
            record = {'type': type_id, 'graph': builder}
            if builder is None:
                record.update(route='excluded', reason=reason)
                records.append(record)
                continue
            try:
                gpu = render(gpu_route, work, by_id[type_id], builder, 'gpu')
                cpu = render(cpu_route, work, by_id[type_id], builder, 'cpu')
                if cpu['provenance'] != 'cpu-reference':
                    raise SweepError('the reference render did not run on the CPU reference path')
                parity, largest = compare_frames(gpu['path'], cpu['path'])
                if (builder == 'chain' and type_id != SOURCE and
                        type_id not in identity_at_defaults and gpu['digest'] == probe['digest']):
                    raise SweepError('passes its input through unchanged with its default '
                                     'parameters; a node that changes pixels must change the '
                                     'gradient, or node_graphs.json must declare under '
                                     'identityAtDefaults why its defaults are the identity')
                record.update(route=gpu['provenance'], gpu=gpu['digest'], cpu=cpu['digest'],
                              parity=parity, largestDifference=largest)
                if gpu['provenance'] != 'gpu':
                    record['reason'] = gpu.get('routeNote') or ''
                for path in (gpu['path'], cpu['path']):
                    os.remove(path)
            except SweepError as error:
                record.update(route='failed', reason=str(error))
            records.append(record)
            print(f'{type_id}: {record["route"]} {record.get("parity", "")} '
                  f'{record.get("reason", "")[:160]}'.rstrip(), flush=True)

    if arguments.records:
        with open(arguments.records, 'w', encoding='utf-8') as file:
            for record in records:
                file.write(json.dumps(record, sort_keys=True) + '\n')

    rows = {}
    for record in records:
        type_id = record['type']
        if record['route'] == 'excluded':
            continue
        if record['route'] == 'failed':
            failures.append(f'{type_id}: {record["reason"]}')
            continue
        if record['parity'] == 'differs':
            failures.append(f'{type_id}: GPU and CPU differ by {record["largestDifference"]:.3g}, '
                            'beyond the 2e-6 gate')
        rows[type_id] = {'route': record['route'], 'gpu': record['gpu'], 'cpu': record['cpu'],
                         'parity': record['parity']}
        expected = baseline['nodes'].get(type_id)
        if arguments.update_baseline:
            continue
        if expected is None:
            failures.append(f'{type_id}: no baseline row (run with --update-baseline and review)')
            continue
        for field, what in (('route', 'route'), ('parity', 'parity'), ('gpu', 'GPU digest'),
                            ('cpu', 'CPU digest')):
            if rows[type_id][field] != expected.get(field):
                failures.append(f'{type_id}: {what} is {rows[type_id][field]}, the baseline has '
                                f'{expected.get(field)}')
    if not arguments.only and not arguments.update_baseline:
        for type_id in sorted(set(baseline['nodes']) - set(rows)):
            if type_id not in {record['type'] for record in records
                               if record['route'] == 'failed'}:
                failures.append(f'{type_id}: in the baseline, but not a registered node type '
                                'with an image output any more')
    if arguments.update_baseline and not arguments.only:
        with open(arguments.baseline, 'w', encoding='utf-8') as file:
            json.dump({'schema': 1, 'source': SOURCE, 'frame': FRAME,
                       'extent': [WIDTH, HEIGHT], 'nodes': rows},
                      file, indent=1, sort_keys=True)
            file.write('\n')
        print(f'baseline written: {len(rows)} nodes')

    for failure in failures:
        print(f'FAIL: {failure}')
    swept = sum(1 for record in records if record['route'] not in ('excluded', 'failed'))
    print(f'node sweep: {swept} rendered, '
          f'{sum(1 for record in records if record["route"] == "excluded")} excluded by '
          f'declaration, {len(failures)} failures')
    return 1 if failures else 0


# --------------------------------------------------------------------------------------------
# Two platforms' baselines against each other


def compare_baselines(first_path, second_path, exceptions_path):
    with open(first_path, encoding='utf-8') as file:
        first = json.load(file)['nodes']
    with open(second_path, encoding='utf-8') as file:
        second = json.load(file)['nodes']
    exceptions = {}
    if exceptions_path:
        with open(exceptions_path, encoding='utf-8') as file:
            exceptions = json.load(file)['nodes']
    failures = []
    for type_id in sorted(set(first) | set(second)):
        if type_id not in first or type_id not in second:
            failures.append(f'{type_id}: in one baseline only')
            continue
        a, b = first[type_id], second[type_id]
        if a['route'] != b['route']:
            failures.append(f'{type_id}: route {a["route"]} on one platform, {b["route"]} on the '
                            'other')
        # The CPU reference is the same everywhere; nothing is excused from that.
        if a['cpu'] != b['cpu']:
            failures.append(f'{type_id}: the CPU reference differs between the platforms')
        # A GPU frame may differ between devices only for an operation whose documented
        # tolerance allows it, and then each platform's own sweep holds it to that gate.
        if a['gpu'] != b['gpu'] and type_id not in exceptions:
            failures.append(f'{type_id}: the GPU frames differ between the platforms and '
                            'cross_platform.json does not list the node')
    for type_id in sorted(set(exceptions) - set(first) - set(second)):
        failures.append(f'{type_id}: listed in cross_platform.json but in neither baseline')
    for failure in failures:
        print(f'FAIL: {failure}')
    print(f'baselines compared: {len(set(first) & set(second))} nodes, {len(failures)} failures')
    return 1 if failures else 0


# --------------------------------------------------------------------------------------------
# The sweep's own rules, without a device


def self_test():
    def node(type_id, inputs, outputs=('image',)):
        return {'id': type_id,
                'inputs': [{'port': name, 'kind': kind, 'required': required}
                           for name, kind, required in inputs],
                'outputs': [{'port': kind, 'kind': kind} for kind in outputs]}

    checks = []
    plain = node('test.plain', [('image', 'image', True)])
    undeclared = node('test.undeclared', [('channels', 'channel-set', True)])
    no_image = node('test.scalar', [], outputs=('scalar',))
    checks.append(('a type the default graph can feed is swept with it',
                   plan([plain, no_image], {}) == {'test.plain': ('chain', '')}))
    try:
        plan([plain, undeclared], {})
        refused = False
    except SweepError as error:
        refused = 'declares no graph' in str(error)
    checks.append(('a new type with no declared graph fails the sweep', refused))
    checks.append(('a declared graph is used',
                   plan([undeclared], {'test.undeclared': {'graph': 'channels'}}) ==
                   {'test.undeclared': ('channels', '')}))
    checks.append(('a declared exclusion carries its reason',
                   plan([undeclared], {'test.undeclared': {'excluded': 'why'}}) ==
                   {'test.undeclared': (None, 'why')}))
    try:
        plan([plain], {'test.gone': {'graph': 'chain'}})
        stale = False
    except SweepError:
        stale = True
    checks.append(('a declaration for a type the registry lacks fails', stale))

    with tempfile.TemporaryDirectory(prefix='bloom-node-sweep-') as work:
        first, second = os.path.join(work, 'a.png'), os.path.join(work, 'b.png')
        write_png(first)
        write_png(second)
        with open(first, 'rb') as a, open(second, 'rb') as b:
            data = a.read()
            checks.append(('the generated image is the same bytes every time',
                           data == b.read() and hashlib.sha256(data).hexdigest() ==
                           PNG_DIGEST))

        def baseline(path, nodes):
            with open(path, 'w', encoding='utf-8') as file:
                json.dump({'nodes': nodes}, file)
            return path

        row = {'route': 'gpu', 'gpu': 'g', 'cpu': 'c', 'parity': 'identical'}
        same = baseline(os.path.join(work, 'same.json'), {'n': row})
        other_gpu = baseline(os.path.join(work, 'gpu.json'), {'n': dict(row, gpu='h')})
        other_cpu = baseline(os.path.join(work, 'cpu.json'), {'n': dict(row, cpu='d')})
        listed = baseline(os.path.join(work, 'listed.json'), {'n': 'documented tolerance'})
        checks.append(('equal baselines agree', compare_baselines(same, same, None) == 0))
        checks.append(('a GPU digest that differs is refused unless the node is listed',
                       compare_baselines(same, other_gpu, None) == 1 and
                       compare_baselines(same, other_gpu, listed) == 0))
        checks.append(('a CPU digest that differs is refused even when listed',
                       compare_baselines(same, other_cpu, listed) == 1))

    failed = [name for name, passed in checks if not passed]
    for name in failed:
        print(f'FAIL: {name}')
    print(f'node sweep self-test: {len(checks) - len(failed)} of {len(checks)} checks passed')
    return 1 if failed else 0


PNG_DIGEST = '9e06281d5c2d647f48667730e191d7f001b4bfae981c4547098d2141ef464658'


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n', 1)[0])
    parser.add_argument('--mcp')
    parser.add_argument('--baseline')
    parser.add_argument('--require-device', action='store_true')
    parser.add_argument('--update-baseline', action='store_true')
    parser.add_argument('--records')
    parser.add_argument('--only', default='')
    parser.add_argument('--compare', nargs=2, metavar='BASELINE')
    parser.add_argument('--cross-platform')
    parser.add_argument('--self-test', action='store_true')
    arguments = parser.parse_args()
    try:
        if arguments.self_test:
            return self_test()
        if arguments.compare:
            return compare_baselines(arguments.compare[0], arguments.compare[1],
                                     arguments.cross_platform)
        if not arguments.mcp or not arguments.baseline:
            parser.error('--mcp and --baseline are required for a sweep')
        return sweep(arguments)
    except SweepError as error:
        print(f'FAIL: {error}')
        return 1


if __name__ == '__main__':
    sys.exit(main())
