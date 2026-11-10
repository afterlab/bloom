"""Python/CLI conformance against built artifacts, without user site imports."""
import ast
import concurrent.futures
import json
import keyword
import hashlib
import inspect
import os
import pathlib
import pydoc
import subprocess
import sys
import tempfile
import threading
import time
import unittest
sys.path.insert(0, sys.argv.pop(1))
CLI = sys.argv.pop(1)
import _bloom
import bloom
from bloom import _state

STUB_TYPES = {"bool": "bool", "int": "int", "float": "float", "str": "str",
              "id": "int | Proxy", "vec2": "tuple[float, float]",
              "vec3": "tuple[float, float, float]",
              "color4": "tuple[float, float, float, float]", "array": "tuple",
              "time": "Time", "value": "Value", "path": "Path"}


def operation_stub(schemas):
    """The exact text of bloom/ops.pyi for a registry, so the stub can never drift from it."""
    lines = ["# Generated from the host operation registry by the bloom.scripting.python-api test.",
             "from collections.abc import Mapping",
             "from fractions import Fraction",
             "from typing import Any",
             "",
             "from .data import Proxy",
             "",
             "Time = int | tuple[int, int] | Fraction | float",
             "Value = float | bool | str | tuple[float, ...] | Mapping[str, Any]",
             "Path = Mapping[str, Any]",
             "",
             "class Operation:",
             "    id: str",
             "    example: str",
             "    schema: Mapping[str, Any]",
             "    def __call__(self, *args: Any, **kwargs: Any) -> Mapping[str, Any] | None: ...",
             ""]
    domains = {}
    for schema in schemas:
        domains.setdefault(schema["id"].split(".")[1], []).append(schema)
    for domain in sorted(domains):
        name = "".join(part.capitalize() for part in domain.split("-")) + "Ops"
        lines.append(f"class {name}:")
        for schema in sorted(domains[domain], key=lambda item: item["id"]):
            verb = schema["id"].split(".")[2].replace("-", "_")
            if keyword.iskeyword(verb):
                verb += "_"
            params = ["self"]
            for argument in schema["arguments"]:
                if argument["contextual"]:
                    continue
                default = "" if argument["required"] else " = ..."
                params.append(f"{argument['name']}: {STUB_TYPES[argument['kind']]}{default}")
            contextual = [a for a in schema["arguments"] if a["contextual"]]
            if contextual:
                params.append("*")
                params += [f"{a['name']}: {STUB_TYPES[a['kind']]} = ..." for a in contextual]
            joined = ", ".join(params)
            lines.append(f"    def {verb}({joined}) -> Mapping[str, Any] | None:")
            # The full reference entry, the text bloom.help() and MCP help print.
            _, reference = _bloom.reference(schema["id"], False)
            reference = reference.rstrip("\n").replace("\\", "\\\\").replace('"""', '\\"""')
            body = "\n".join(("        " + line).rstrip() for line in reference.split("\n"))
            lines.append('        """' + body.lstrip() + "\n        " + '"""')
            lines.append("        ...")
        lines += ["", f"{domain.replace('-', '_')}: {name}", ""]
    lines += ["def registry() -> Mapping[str, Operation]: ...",
              "def get(operation_id: str) -> Operation: ..."]
    return "\n".join(lines) + "\n"


class PythonApi(unittest.TestCase):
    def setUp(self):
        self.host = _bloom.Host()
        self.token = _state.bind(self.host)

    def tearDown(self):
        _state._host.reset(self.token)

    def test_registry(self):
        registry = bloom.ops.registry()
        self.assertEqual({s['id'] for s in self.host.schemas()}, set(registry))
        for schema in self.host.schemas():
            _, domain, verb = schema['id'].split('.')
            verb = verb.replace('-', '_')
            name = verb + '_' if keyword.iskeyword(verb) else verb
            self.assertEqual(getattr(getattr(bloom.ops, domain.replace('-', '_')), name).id, schema['id'])
            signature = inspect.signature(registry[schema['id']])
            positional = [a for a in schema['arguments'] if not a['contextual']]
            contextual = [a for a in schema['arguments'] if a['contextual']]
            self.assertEqual(list(signature.parameters),
                             [a['name'] for a in positional + contextual])
            optional_seen = False
            for argument in positional:
                p = signature.parameters[argument['name']]
                self.assertEqual(p.kind, inspect.Parameter.POSITIONAL_OR_KEYWORD)
                self.assertEqual(p.default is inspect.Parameter.empty, argument['required'])
                # Positional calls bind in schema order, so required never follows optional.
                optional_seen = optional_seen or not argument['required']
                self.assertFalse(optional_seen and argument['required'], argument['name'])
            for argument in contextual:
                p = signature.parameters[argument['name']]
                self.assertEqual(p.kind, inspect.Parameter.KEYWORD_ONLY)
                self.assertIsNone(p.default)
            # Every operation publishes a call an artist can paste.
            self.assertTrue(schema['example'].startswith('bloom.ops.'), schema['id'])
            self.assertIsInstance(schema['description'], str, schema['id'])
        with self.assertRaises(bloom.OperationError):
            bloom.ops.layer.add_solid(composition=1, name='bad', color=(0, 0, float('nan'), 1))
        with self.assertRaises(bloom.OperationError):
            bloom.ops.project.set_name(name='bad', unknown=1)
        for field in ('frameRateNumerator', 'frameRateDenominator'):
            for value in (-1, 0, 2**32, 2**63 - 1):
                with self.subTest(field=field, value=value), self.assertRaises(bloom.OperationError):
                    bloom.ops.composition.add(name='Invalid rate', width=64, height=64,
                                              duration=(48, 1), **{field: value})
        self.assertEqual(bloom.data.snapshot()['revision'], 0)

    def test_every_operation_is_constructible(self):
        """SCRIPT-1's registry had 69 ids with no argument schema at all; none may remain."""
        for schema in self.host.schemas():
            self.assertTrue(schema['arguments'], schema['id'])
            for argument in schema['arguments']:
                self.assertTrue(argument['summary'], f"{schema['id']}.{argument['name']}")
                self.assertIn(argument['kind'], STUB_TYPES)
                self.assertIsNotNone(argument['example'], f"{schema['id']}.{argument['name']}")

    def test_context_defaults(self):
        """The first three minutes: omit composition, time and selection and they come from context."""
        composition = bloom.context.composition
        self.assertIsNotNone(composition)
        result = bloom.ops.layer.add_solid(name='Red', color=(1, 0, 0, 1))
        self.assertTrue(result['succeeded'])
        layer = next(o['id'] for o in result['outputs'] if o['name'] == 'layer')
        # Contextual arguments stay keyword-only, so an explicit target reads as one.
        self.assertTrue(
            bloom.ops.layer.rename('Renamed', layer=layer, composition=composition)['succeeded'])
        self.assertTrue(bloom.ops.layer.set_enabled(False, layer=layer)['succeeded'])
        # Schema and context agree about which arguments default and where from.
        schema = bloom.ops.get('bloom.layer.set-enabled').schema
        defaults = {a['name']: a['context'] for a in schema['arguments'] if a['contextual']}
        self.assertEqual(defaults, {'composition': 'composition', 'layer': 'layer'})
        for source in defaults.values():
            self.assertIn(source, bloom.context.DEFAULTS)

    def test_positional_and_proxy_arguments(self):
        composition = bloom.context.composition
        self.assertTrue(bloom.ops.layer.add_solid('Red', (1, 0, 0, 1))['succeeded'])
        self.assertTrue(bloom.ops.layer.add_solid('Green', [0, 1, 0, 1], (10, 20))['succeeded'])
        # Proxies and raw ids are interchangeable for id arguments.
        self.assertTrue(bloom.ops.composition.set_name('By proxy', composition=composition)['succeeded'])
        self.assertTrue(bloom.ops.composition.set_name('By id', composition=int(composition))['succeeded'])
        self.assertEqual(bloom.context.composition.name, 'By id')
        # Times accept whole frames, exact pairs, Fractions and floats.
        from fractions import Fraction
        for moment in (12, (1, 2), Fraction(3, 4), 1.5):
            with self.subTest(time=moment):
                self.assertTrue(bloom.ops.composition.set_duration(moment)['succeeded'])

    def test_composition_settings(self):
        """#376: Composition Settings is one registry operation; omitted arguments keep their value."""
        composition = bloom.context.composition
        revision = bloom.data.snapshot()['revision']
        result = bloom.ops.composition.set_settings(
            name='Settings', width=1280, height=720, frameRateNumerator=30000,
            frameRateDenominator=1001, dropFrame=True, anchor=0)
        self.assertTrue(result['succeeded'])
        self.assertEqual(bloom.data.snapshot()['revision'], revision + 1)
        self.assertEqual((composition.name, composition.width, composition.height),
                         ('Settings', 1280, 720))
        self.assertEqual(composition.frame_rate, (30000, 1001))
        with self.assertRaises(bloom.OperationError):
            bloom.ops.composition.set_settings(actionSafe=80.0, titleSafe=90.0)
        with self.assertRaises(bloom.OperationError):
            bloom.ops.composition.set_settings(anchor=9)
        self.assertEqual(bloom.data.snapshot()['revision'], revision + 1)

    def test_asset_folders(self):
        """#378: compositions and folders move between Assets folders as single operations."""
        def folder(name, **kwargs):
            result = bloom.ops.asset.create_folder(name, **kwargs)
            return next(o['id'] for o in result['outputs'] if o['name'] == 'folder')
        comps = folder('Comps')
        nested = folder('Nested', parent=comps)
        composition = bloom.context.composition
        revision = bloom.data.snapshot()['revision']
        self.assertTrue(bloom.ops.asset.move_compositions([composition], folder=comps)['succeeded'])
        self.assertTrue(bloom.ops.asset.move_folders([nested])['succeeded'])
        self.assertEqual(bloom.data.snapshot()['revision'], revision + 2)
        with self.assertRaises(bloom.OperationError):
            bloom.ops.asset.move_folders([comps], parent=comps)
        self.assertTrue(bloom.ops.asset.move_compositions([composition])['succeeded'])

    def test_owner_first_minutes(self):
        """The three lines from the owner's first session in the Script editor."""
        # 1. No composition argument: the current composition is the default.
        with self.assertRaises(bloom.OperationError) as error:
            bloom.ops.layer.add_solid()
        message = str(error.exception)
        self.assertIn('bloom.layer.add-solid', message)
        self.assertIn("'name'", message)
        self.assertIn('Example: bloom.ops.layer.add_solid(name="Red", color=(1, 0, 0, 1))', message)
        self.assertNotIn('keyword-only', message)
        # 2. A positional call is accepted, and a wrong type says what was expected.
        with self.assertRaises(bloom.OperationError) as error:
            bloom.ops.layer.add_solid(1)
        message = str(error.exception)
        self.assertIn("argument 'name' expects text, got int", message)
        self.assertIn('Example: bloom.ops.layer.add_solid', message)
        self.assertTrue(bloom.ops.layer.add_solid('Red', (1, 0, 0, 1))['succeeded'])
        # 3. The context repr summarises the session; no owner pointer leaks.
        text = repr(bloom.context)
        self.assertTrue(text.startswith('<bloom.context '), text)
        for field in ('project=', 'composition=', 'selection=', 'time='):
            self.assertIn(field, text)
        self.assertNotIn('_owner', text)
        self.assertNotIn('Host object', text)

    def test_layers_are_addressable(self):
        """Layer operations need a LayerId, so the snapshot has to publish one."""
        result = bloom.ops.layer.add_solid('Red', (1, 0, 0, 1))
        layer = next(o['id'] for o in result['outputs'] if o['name'] == 'layer')
        composition = bloom.context.composition
        found = bloom.data.layers(composition).get(layer)
        self.assertEqual(found.name, 'Red')
        self.assertEqual(repr(found), f"<Layer {layer} 'Red'>")
        self.assertTrue(bloom.ops.layer.rename('Renamed', layer=found)['succeeded'])
        self.assertEqual(bloom.data.layers(composition).get(layer).name, 'Renamed')
        # Headless runs have no selection, so a layer argument stays unanswered and says so.
        self.assertEqual(bloom.context.layers, ())
        with self.assertRaises(bloom.OperationError) as error:
            bloom.ops.layer.set_enabled(False)
        self.assertIn('bloom.context', str(error.exception))

    def test_names_roles_and_set_keys(self):
        """Layers by name, parameters by layer and role, string ids and one-step keys."""
        bloom.ops.layer.add_solid('Wash', (1, 0, 0, 1))
        bloom.ops.layer.add_solid('Twin', (0, 1, 0, 1))
        bloom.ops.layer.add_solid('Twin', (0, 0, 1, 1))
        composition = bloom.context.composition
        self.assertTrue(bloom.ops.layer.rename('Wash 2', layer={'name': 'Wash'})['succeeded'])
        names = [layer.name for layer in bloom.data.layers(composition)]
        self.assertIn('Wash 2', names)
        self.assertTrue(bloom.ops.layer.rename(
            'Wash', layer=str(next(int(l) for l in bloom.data.layers(composition)
                                   if l.name == 'Wash 2')))['succeeded'])
        with self.assertRaises(bloom.OperationError) as error:
            bloom.ops.layer.rename('X', layer={'name': 'Twin'})
        self.assertIn('2 layers are named "Twin"', str(error.exception))
        with self.assertRaises(bloom.OperationError) as error:
            bloom.ops.layer.rename('X', layer={'name': 'Missing'})
        self.assertIn('No layer is named "Missing"', str(error.exception))
        keyed = bloom.ops.animation.set_keys(
            {'layer': 'Wash', 'role': 'opacity'}, [(0, 0.0), (12, 1.0, 'hold')])
        self.assertTrue(keyed['succeeded'])
        self.assertTrue(bloom.ops.animation.set_keys(
            {'layer': 'Wash', 'role': 'opacity'}, [], replace=True)['succeeded'])

    def test_easing_and_layer_flags(self):
        """Document 1.30: easing equations, Easy Ease and the shy flag through the registry."""
        result = bloom.ops.layer.add_solid('Red', (1, 0, 0, 1))
        layer = next(o['id'] for o in result['outputs'] if o['name'] == 'layer')
        composition = bloom.context.composition
        opacity = bloom.data.nodes(composition)[-1].parameters['opacity']
        bloom.ops.animation.create_for_parameter(opacity, time=0)
        bloom.ops.animation.set_keyframe_at_time_for_parameter(opacity, 0.0, time=0)
        bloom.ops.animation.set_keyframe_at_time_for_parameter(opacity, 1.0, time=12)
        curve = bloom.data.curves(composition)[-1]
        key = curve.components[0][0]['id']
        self.assertTrue(bloom.ops.animation.set_keyframes_easing(
            keys=((int(curve), key),), interpolation='elastic', ease='out',
            amplitude=2.0)['succeeded'])
        row = bloom.data.curves(composition)[-1].components[0][0]
        self.assertEqual((row['interpolation'], row['interpolation_name'], row['ease']),
                         (12, 'elastic', 'out'))
        self.assertEqual((row['amplitude'], row['back']), (2.0, 1.70158))
        self.assertTrue(bloom.ops.animation.easy_ease(keys=((int(curve), key),))['succeeded'])
        row = bloom.data.curves(composition)[-1].components[0][0]
        self.assertEqual(row['interpolation_name'], 'ease-in-out')
        self.assertTrue(bloom.ops.animation.set_keyframes_interpolation(
            keys=((int(curve), key),), interpolation='constant')['succeeded'])
        self.assertEqual(bloom.data.curves(composition)[-1].components[0][0]['interpolation'], 0)
        found = bloom.data.layers(composition).get(layer)
        self.assertFalse(found.shy)
        self.assertFalse(found.name_follows_source)
        self.assertTrue(bloom.ops.layer.set_shy(True, layer=layer)['succeeded'])
        self.assertTrue(bloom.data.layers(composition).get(layer).shy)

    def test_keyframe_handle_modes(self):
        """Document 1.32: per-key handle modes through the registry, read back in curve rows."""
        bloom.ops.layer.add_solid('Red', (1, 0, 0, 1))
        composition = bloom.context.composition
        opacity = bloom.data.nodes(composition)[-1].parameters['opacity']
        bloom.ops.animation.create_for_parameter(opacity, time=0)
        for frame, value in ((0, 0.0), (12, 1.0), (24, 0.25)):
            bloom.ops.animation.set_keyframe_at_time_for_parameter(opacity, value, time=frame)
        curve = bloom.data.curves(composition)[-1]
        rows = curve.components[0]
        self.assertEqual({row['handle_mode'] for row in rows}, {'free'})
        keys = tuple((int(curve), row['id']) for row in rows)
        self.assertTrue(bloom.ops.animation.set_keyframes_handle_mode(
            keys=keys[1:2], mode='aligned')['succeeded'])
        self.assertEqual([row['handle_mode'] for row in bloom.data.curves(composition)[-1].components[0]],
                         ['free', 'aligned', 'free'])
        before = bloom.data.curves(composition)[-1].components[0][1]
        self.assertTrue(bloom.ops.animation.set_keyframes_handle_mode(
            keys=keys[1:2], mode='auto')['succeeded'])
        after = bloom.data.curves(composition)[-1].components[0][1]
        self.assertEqual(after['handle_mode'], 'auto')
        self.assertNotEqual(before['outgoing_handle'], after['outgoing_handle'])
        with self.assertRaises(Exception):
            bloom.ops.animation.set_keyframes_handle_mode(keys=keys[1:2], mode='smooth')

    def test_align_and_distribute_layers(self):
        """ADR 0031: the ops measure the layers headlessly and move them in one undoable step."""
        layers = []
        for name, x in (('A', 300), ('B', 900), ('C', 1500)):
            result = bloom.ops.layer.add_solid(name, (1, 1, 1, 1), position=(x, 540))
            layers.append(next(o['id'] for o in result['outputs'] if o['name'] == 'layer'))
        revision = bloom.data.snapshot()['revision']
        result = bloom.ops.layer.align_layers('left', target='composition', layers=layers)
        self.assertTrue(result['succeeded'])
        moved = {o['id'] for o in result['outputs'] if o['name'] == 'movedLayer'}
        self.assertEqual(moved, set(layers))
        self.assertEqual(bloom.data.snapshot()['revision'], revision + 1)
        again = bloom.ops.layer.align_layers('left', target='composition', layers=layers)
        self.assertTrue(again['succeeded'])
        self.assertFalse([o for o in again['outputs'] if o['name'] == 'movedLayer'])
        with self.assertRaises(bloom.OperationError):
            bloom.ops.layer.distribute_layers('horizontal', layers=layers[:2])
        with self.assertRaises(bloom.OperationError) as error:
            bloom.ops.layer.align_layers('left', target='key-object', layers=layers)
        self.assertIn("'key'", str(error.exception))
        self.assertTrue(bloom.ops.layer.distribute_layers(
            'vertical', target='key-object', key=layers[0], layers=layers)['succeeded'])

    def test_reprs(self):
        self.assertEqual(repr(bloom.context.composition), "<Composition 1 'Composition'>")
        self.assertEqual(repr(bloom.context.project), "<Project 1 'Untitled'>")
        node = bloom.data.nodes(1)[0]
        self.assertRegex(repr(node), r"^<Node \d+ '[^']+'>$")
        self.assertNotIn('_owner', repr(node))
        bloom.ops.node.add(nodeType='bloom.value-scalar', position=(0, 0))
        added = bloom.data.nodes(1)[-1]
        bloom.ops.node.remove(nodes=(added.id,))
        self.assertEqual(repr(added), f'<Node {added.id} (stale)>')

    def test_help_and_dir(self):
        document = pydoc.render_doc(bloom.ops.layer.add_solid).replace('\b', '')
        for fragment in ('bloom.ops.layer.add_solid', 'Arguments:', 'The layer name.',
                         'defaults to bloom.context.composition', 'Example:',
                         'bloom.ops.layer.add_solid(name="Red", color=(1, 0, 0, 1))'):
            self.assertIn(fragment, document)
        names = dir(bloom.ops)
        for domain in ('animation', 'asset', 'composition', 'data_block', 'layer', 'merge',
                       'node', 'node_group', 'parameter', 'project'):
            self.assertIn(domain, names)
        self.assertEqual(names, sorted(names))
        self.assertIn('add_solid', dir(bloom.ops.layer))
        self.assertIn('bloom.ops.layer', repr(bloom.ops.layer))

    def test_stubs_parse(self):
        for stub in pathlib.Path(bloom.__file__).parent.glob('*.pyi'):
            ast.parse(stub.read_text(), filename=str(stub))

    def test_operation_stub_matches_registry(self):
        stub = pathlib.Path(bloom.__file__).parent / 'ops.pyi'
        self.assertEqual(stub.read_text(), operation_stub(self.host.schemas()),
                         'regenerate bloom/ops.pyi from the registry')

    def test_cli_applies_the_same_context_defaults(self):
        with tempfile.TemporaryDirectory(prefix='bloom-cli-context-') as directory:
            script = pathlib.Path(directory) / 'script.json'
            script.write_text(json.dumps([
                {'op': 'bloom.layer.add-solid',
                 'args': {'name': 'Red', 'color': [1, 0, 0, 1]}},
                {'op': 'bloom.composition.set-name', 'args': {'name': 'From context'}}]))
            command = subprocess.run([CLI, 'run', str(script)], capture_output=True, timeout=60)
            self.assertEqual(command.returncode, 0, command.stderr.decode())
            self.assertIn(b'executed 2 transaction(s)', command.stdout)

    def test_cli_imports_an_svg_in_either_mode(self):
        """#642: bloom-cli run takes `mode` on bloom.layer.add-svg (omitted: one layer), refuses an
        invalid one by name, and bloom.layer.break-apart-svg turns the one-layer artwork into shapes."""
        with tempfile.TemporaryDirectory(prefix='bloom-cli-svg-') as directory:
            svg = pathlib.Path(directory) / 'art.svg'
            svg.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">'
                           '<rect x="10" y="10" width="80" height="60" fill="#c33"/>'
                           '<circle cx="150" cy="50" r="30" fill="#36c"/></svg>')

            def run(name, edits):
                script = pathlib.Path(directory) / name
                script.write_text(json.dumps(edits))
                return subprocess.run([CLI, 'run', str(script)], capture_output=True, timeout=60)

            omitted = run('layer.json', [{'op': 'bloom.layer.add-svg', 'args': {'path': str(svg)}}])
            self.assertEqual(omitted.returncode, 0, omitted.stderr.decode())
            shapes = run('shapes.json', [{'op': 'bloom.layer.add-svg',
                                          'args': {'path': str(svg), 'mode': 'shapes'}}])
            self.assertEqual(shapes.returncode, 0, shapes.stderr.decode())
            both = run('break.json', [{'op': 'bloom.layer.add-svg', 'args': {'path': str(svg), 'mode': 'layer'}},
                                      {'op': 'bloom.layer.break-apart-svg', 'args': {'layer': 1}}])
            self.assertEqual(both.returncode, 0, both.stderr.decode())
            self.assertIn(b'executed 2 transaction(s)', both.stdout)
            bad = run('bad.json', [{'op': 'bloom.layer.add-svg', 'args': {'path': str(svg), 'mode': 'flat'}}])
            self.assertNotEqual(bad.returncode, 0)
            self.assertIn(b"argument 'mode'", bad.stderr)
            self.assertIn(b'"shapes"', bad.stderr)
            # A layer that holds no one-layer SVG is refused (the CLI reports the rejected operation;
            # the rejection's reason is asserted in bloom.scripting.svg-import-modes).
            none = run('none.json', [{'op': 'bloom.layer.add-solid', 'args': {'name': 'Red', 'color': [1, 0, 0, 1]}},
                                     {'op': 'bloom.layer.break-apart-svg', 'args': {'layer': 1}}])
            self.assertNotEqual(none.returncode, 0)
            self.assertIn(b'bloom.layer.break-apart-svg', none.stderr)
            # The refusal's reason is printed, not only that the operation was refused.
            self.assertIn(b'does not hold an SVG as one Vector Artwork layer', none.stderr)

    def test_cli_adds_an_imported_svg_asset_again(self):
        """#646: bloom-cli run adds an imported SVG asset to a composition again as artwork (mode as
        for bloom.layer.add-svg), and bloom.layer.add-image refuses an SVG asset. (A script cannot delete a file between its own
        operations; the missing-file refusal by name is asserted in bloom.scripting.svg-import-modes.)"""
        with tempfile.TemporaryDirectory(prefix='bloom-cli-readd-') as directory:
            def write(name):
                svg = pathlib.Path(directory) / name
                svg.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">'
                               '<rect x="10" y="10" width="80" height="60" fill="#c33"/></svg>')
                return svg

            def run(name, edits):
                script = pathlib.Path(directory) / name
                script.write_text(json.dumps(edits))
                return subprocess.run([CLI, 'run', str(script)], capture_output=True, timeout=60)

            # One layer: the project holds the SVG, so the file can go; both modes add again.
            held = write('held.svg')
            first = [{'op': 'bloom.layer.add-svg', 'args': {'path': str(held), 'mode': 'layer'}}]
            for mode in ('layer', 'shapes'):
                again = run('again-%s.json' % mode, first + [
                    {'op': 'bloom.layer.add-from-assets', 'args': {'assets': [1], 'mode': mode}}])
                self.assertEqual(again.returncode, 0, again.stderr.decode())
                self.assertIn(b'executed 2 transaction(s)', again.stdout)
            # The asset is added again, never as an image layer.
            image = run('image.json', first + [{'op': 'bloom.layer.add-image', 'args': {'asset': 1}}])
            self.assertNotEqual(image.returncode, 0)
            self.assertIn(b'is an SVG, not footage', image.stderr)
            # A bad mode is named.
            bad = run('bad-mode.json', first + [
                {'op': 'bloom.layer.add-from-assets', 'args': {'assets': [1], 'mode': 'flat'}}])
            self.assertNotEqual(bad.returncode, 0)
            self.assertIn(b"argument 'mode'", bad.stderr)

    def test_cli_export_renders_the_saved_queue(self):
        """ADR 0033: bloom-cli export renders a saved queue headlessly and reports the result."""
        if sys.platform != 'linux':
            self.skipTest('export runs on Linux in this version')
        composition = bloom.context.composition
        self.assertTrue(bloom.ops.layer.add_solid('Red', (1, 0, 0, 1))['succeeded'])
        settings = {'format': 'png', 'codecProfile': '', 'compression': '',
                    'outputColorSpace': '', 'display': '', 'view': '', 'bakeLook': True,
                    'includeAudio': False, 'audioSampleRate': 48000, 'encoder': 'automatic',
                    'naming': {'framePadding': 4, 'pattern': '<base>.####.<ext>'}}
        output = {'preset': 'builtin:png-sequence', 'settings': settings,
                  'destination': {'portability': 'project-relative',
                                  'path': 'Exports/{name}.png', 'overwrite': 'replace'},
                  'enabled': True, 'importIntoAssets': False}
        added = bloom.ops.export.add_job(composition=composition, range='frame', first=3,
                                         outputs=(json.dumps(output),))
        self.assertTrue(added['succeeded'])
        with tempfile.TemporaryDirectory(prefix='bloom-cli-export-') as directory:
            project = pathlib.Path(directory) / 'shot.bloom'
            self.host.save(str(project))
            command = subprocess.run([CLI, 'export', '--project', str(project), '--all'],
                                     capture_output=True, timeout=180)
            self.assertEqual(command.returncode, 0, command.stderr.decode())
            stills = list((pathlib.Path(directory) / 'Exports').glob('*.png'))
            self.assertEqual(len(stills), 1, command.stdout.decode())
            self.assertEqual(stills[0].read_bytes()[:8], b'\x89PNG\r\n\x1a\n')
            self.assertIn(b'succeeded', command.stdout)
            unknown = subprocess.run([CLI, 'export', '--project', str(project), '--job', '999'],
                                     capture_output=True, timeout=180)
            self.assertNotEqual(unknown.returncode, 0)
            # A missing job named beside a real one is refused, not skipped while the rest run.
            job = next(o['id'] for o in added['outputs'] if o['name'] == 'exportJob')
            mixed = subprocess.run([CLI, 'export', '--project', str(project), '--job', str(job),
                                    '--job', '999'], capture_output=True, timeout=180)
            self.assertNotEqual(mixed.returncode, 0)
            self.assertIn(b'The job does not exist: 999', mixed.stderr)

    def test_a_compound_path_value_round_trips(self):
        """#589: a Path node's value takes contours, a fill rule and keyed groups from Python."""
        composition = bloom.context.composition
        def square(left, size):
            return {'closed': True, 'anchors': [
                {'point': (left, left)}, {'point': (left + size, left)},
                {'point': (left + size, left + size)}, {'point': (left, left + size)}]}
        result = bloom.ops.node.add(nodeType='bloom.value-path', position=(0, 0))
        self.assertTrue(result['succeeded'])
        node = next(o['id'] for o in result['outputs'] if o['name'] == 'node')
        parameter = bloom.data.nodes(composition).get(node).parameters['value']
        compound = {'contours': [square(0, 40), square(10, 10)], 'fill_rule': 'evenOdd',
                    'groups': [{'key': 'outer', 'first_contour': 0, 'contour_count': 1,
                                'color': (1, 0, 0, 1)}]}
        self.assertTrue(bloom.ops.parameter.set_source(parameter, value=compound)['succeeded'])
        value = bloom.data.parameters(composition).get(parameter).value
        self.assertEqual(value['fill_rule'], 'evenOdd')
        self.assertEqual(len(value['contours']), 2)
        self.assertEqual(value['contours'][1]['anchors'][0]['point'], (10.0, 10.0))
        self.assertEqual(value['groups'][0]['key'], 'outer')
        self.assertEqual(value['groups'][0]['first_contour'], 0)
        self.assertEqual(value['groups'][0]['color'], (1.0, 0.0, 0.0, 1.0))
        # A one-contour value reads back in the shape it always had.
        self.assertTrue(bloom.ops.parameter.set_source(parameter, value=square(0, 4))['succeeded'])
        value = bloom.data.parameters(composition).get(parameter).value
        self.assertTrue(value['closed'])
        self.assertEqual(len(value['anchors']), 4)
        self.assertNotIn('contours', value)

    def test_layer_masks_take_path_values(self):
        """ADR 0028: a path is a value kind; masks are added, edited and keyed from Python and the CLI."""
        composition = bloom.context.composition
        added = bloom.ops.layer.add_solid('Plate', (1, 1, 1, 1))
        layer = next(o['id'] for o in added['outputs'] if o['name'] == 'layer')
        square = {'closed': True, 'anchors': [
            {'point': (0, 0)}, {'point': (10, 0), 'out_handle': (12, 2)},
            {'point': (10, 10), 'in_handle': None}, {'point': (0, 10)}]}
        result = bloom.ops.layer.add_mask(square, layer=layer)
        self.assertTrue(result['succeeded'])
        outputs = {o['name']: o['id'] for o in result['outputs']}
        mask, path = outputs['mask'], outputs['path']
        masks = bloom.data.layers(composition).get(layer).masks
        self.assertEqual([m['id'] for m in masks], [mask])
        self.assertEqual(masks[0]['path'], path)
        self.assertEqual(masks[0]['mode'], 1)
        value = bloom.data.parameters(composition).get(path).value
        self.assertTrue(value['closed'])
        self.assertEqual(value['anchors'][1]['out_handle'], (12.0, 2.0))
        self.assertIsNone(value['anchors'][1]['in_handle'])
        # Each anchor names its handle mode; an omitted mode is the one its handles imply.
        self.assertEqual([anchor['mode'] for anchor in value['anchors']],
                         ['corner', 'free', 'corner', 'corner'])
        auto = {'closed': True, 'anchors': [dict(anchor, mode='auto') if index == 1 else anchor
                                            for index, anchor in enumerate(value['anchors'])]}
        self.assertTrue(bloom.ops.layer.set_mask_path(mask, auto, layer=layer)['succeeded'])
        self.assertEqual(
            bloom.data.parameters(composition).get(path).value['anchors'][1]['mode'], 'auto')
        with self.assertRaises(bloom.OperationError):
            bloom.ops.layer.set_mask_path(
                mask, {'closed': True, 'anchors': [{'point': (0, 0), 'mode': 'smooth'}]},
                layer=layer)
        # A path read back from the snapshot passes straight back in.
        moved = {'closed': value['closed'],
                 'anchors': [dict(anchor, point=(anchor['point'][0] + 5, anchor['point'][1]))
                             for anchor in value['anchors']]}
        self.assertTrue(bloom.ops.layer.set_mask_path(mask, moved, layer=layer)['succeeded'])
        self.assertEqual(bloom.data.parameters(composition).get(path).value['anchors'][0]['point'],
                         (5.0, 0.0))
        # #434 O2: parameter.set_source takes the same path object for a Path parameter.
        self.assertTrue(bloom.ops.parameter.set_source(path, value=square)['succeeded'])
        self.assertEqual(bloom.data.parameters(composition).get(path).value['anchors'][0]['point'],
                         (0.0, 0.0))
        self.assertTrue(bloom.ops.parameter.set_source(path, value=moved)['succeeded'])
        self.assertTrue(bloom.ops.layer.animate_mask_path(mask, layer=layer, time=0)['succeeded'])
        self.assertTrue(
            bloom.ops.layer.set_mask_path(mask, square, layer=layer, time=12)['succeeded'])
        self.assertEqual(bloom.data.parameters(composition).get(path).source, 'curve')
        self.assertTrue(
            bloom.ops.layer.stop_animating_mask_path(mask, square, layer=layer)['succeeded'])
        self.assertEqual(bloom.data.parameters(composition).get(path).source, 'constant')
        with self.assertRaises(bloom.OperationError) as error:
            bloom.ops.layer.add_mask({'closed': True, 'anchors': [{'point': (0, float('nan'))}]},
                                     layer=layer)
        self.assertIn("argument 'path' expects a path", str(error.exception))
        self.assertIn("bloom.ops.layer.add_mask(path={\"closed\": True", str(error.exception))
        # The CLI takes the same path as a JSON object; a fresh project allocates the same ids.
        with tempfile.TemporaryDirectory(prefix='bloom-cli-mask-') as directory:
            script = pathlib.Path(directory) / 'script.json'
            script.write_text(json.dumps([
                {'op': 'bloom.layer.add-solid', 'args': {'name': 'Plate', 'color': [1, 1, 1, 1]}},
                {'op': 'bloom.layer.add-mask', 'args': {'layer': layer, 'path': {
                    'closed': True, 'anchors': [
                        {'point': [0, 0], 'inHandle': None, 'outHandle': [2, 0]},
                        {'point': [8, 0]}, {'point': [8, 8]}]}}}]))
            command = subprocess.run([CLI, 'run', str(script)], capture_output=True, timeout=60)
            self.assertEqual(command.returncode, 0, command.stderr.decode())
            self.assertIn(b'executed 2 transaction(s)', command.stdout)

    def test_curve_points_take_auto_anchors_with_null_handles(self):
        """#547: a Curves point list takes an auto anchor with null handles; a bad one is named."""
        before = {node.id for node in bloom.data.nodes(1)}
        bloom.ops.node.add(composition=1, nodeType='bloom.curves', position=(0, 0))
        node = next(node for node in bloom.data.nodes(1) if node.id not in before)
        curve = node.parameters['master']
        points = [{'point': (0, 0), 'mode': 'auto', 'in_handle': None, 'out_handle': None},
                  {'point': (0.5, 0.7), 'mode': 'auto', 'in_handle': None, 'out_handle': None},
                  {'point': (1, 1), 'mode': 'corner'}]
        self.assertTrue(bloom.ops.parameter.set_source(
            curve, value={'closed': False, 'anchors': points})['succeeded'])
        value = bloom.data.parameters(1).get(curve).value
        self.assertEqual([row['point'] for row in value['anchors']],
                         [(0.0, 0.0), (0.5, 0.7), (1.0, 1.0)])
        self.assertEqual({row['mode'] for row in value['anchors']}, {'corner'})
        with self.assertRaises(bloom.OperationError) as raised:
            bloom.ops.parameter.set_source(curve, value={'closed': False, 'anchors': [
                {'point': (0, 0), 'mode': 'auto', 'in_handle': (0.1, 0.1)}]})
        self.assertIn('anchor 0 has a handle', str(raised.exception))
        self.assertIn('point:[input, output]', str(raised.exception))

    def test_locked_layer_takes_view_state_only(self):
        """#470: a locked layer still disables, solos and unlocks; its content edits are refused."""
        result = bloom.ops.layer.add_solid('Locked', (1, 0, 0, 1))
        layer = next(o['id'] for o in result['outputs'] if o['name'] == 'layer')
        self.assertTrue(bloom.ops.layer.set_locked(True, layer=layer)['succeeded'])
        self.assertTrue(bloom.ops.layer.set_enabled(False, layer=layer)['succeeded'])
        self.assertTrue(bloom.ops.layer.set_solo(True, layer=layer)['succeeded'])
        self.assertTrue(bloom.ops.layer.set_shy(True, layer=layer)['succeeded'])
        with self.assertRaises(bloom.OperationError) as raised:
            bloom.ops.layer.set_range(layer=layer, start=1, end=5)
        self.assertIn('locked', str(raised.exception))
        self.assertTrue(bloom.ops.layer.set_locked(False, layer=layer)['succeeded'])
        self.assertTrue(bloom.ops.layer.set_range(layer=layer, start=1, end=5)['succeeded'])

    def test_gradient_blend_modes(self):
        """#546: a gradient's interpolation, colour mode and hue direction read and write by name."""
        before = {node.id for node in bloom.data.nodes(1)}
        bloom.ops.node.add(composition=1, nodeType='bloom.colorize', position=(0, 0))
        node = next(node for node in bloom.data.nodes(1) if node.id not in before)
        gradient = node.parameters['gradient']
        value = bloom.data.parameters(1).get(gradient).value
        self.assertEqual((value['interpolation'], value['color_mode'], value['hue_direction']),
                         ('linear', 'rgb', 'near'))
        blended = {'stops': [{'position': 0.0, 'color': (1, 0, 0, 1)},
                             {'position': 1.0, 'color': (0, 0, 1, 1)}],
                   'interpolation': 'ease', 'color_mode': 'hsl', 'hue_direction': 'far'}
        self.assertTrue(bloom.ops.parameter.set_source(gradient, value=blended)['succeeded'])
        value = bloom.data.parameters(1).get(gradient).value
        self.assertEqual((value['interpolation'], value['color_mode'], value['hue_direction']),
                         ('ease', 'hsl', 'far'))
        # A gradient read back passes straight back in; the modes may be left out.
        self.assertTrue(bloom.ops.parameter.set_source(gradient, value=value)['succeeded'])
        self.assertTrue(bloom.ops.parameter.set_source(
            gradient, value={'stops': blended['stops']})['succeeded'])
        self.assertEqual(bloom.data.parameters(1).get(gradient).value['interpolation'], 'linear')
        with self.assertRaises((ValueError, bloom.OperationError)):
            bloom.ops.parameter.set_source(gradient, value=dict(blended, interpolation='bezier'))

    def test_removed_node_is_stale(self):
        before = {node.id for node in bloom.data.nodes(1)}
        bloom.ops.node.add(composition=1, nodeType='bloom.value-scalar', position=(0, 0))
        node = next(node for node in bloom.data.nodes(1) if node.id not in before)
        bloom.ops.node.remove(composition=1, nodes=(node.id,))
        with self.assertRaises(bloom.StaleObjectError) as error:
            node.resolve()
        self.assertEqual(error.exception.id, node.id)
        self.assertEqual(error.exception.revision, bloom.data.snapshot()['revision'])

    def test_transactions(self):
        before = bloom.data.snapshot()
        with bloom.transactions.group('Two edits'):
            bloom.ops.project.set_name(name='first')
            bloom.ops.composition.set_name(composition=1, name='second')
        self.assertEqual(bloom.data.snapshot()['revision'], before['revision'] + 1)
        bloom.transactions.undo()
        self.assertEqual(bloom.context.project.name, 'Untitled')
        self.assertEqual(bloom.context.composition.name, 'Composition')
        with self.assertRaises(bloom.OperationError):
            with bloom.transactions.group('Conflict', expected_revision=0):
                bloom.ops.project.set_name(name='must not apply')
        with self.assertRaises(ValueError):
            with bloom.transactions.group('Abort'):
                bloom.ops.project.set_name(name='must not apply')
                raise ValueError('abort')
        self.assertEqual(bloom.context.project.name, 'Untitled')

    def test_proxy(self):
        node = bloom.data.nodes(1)[0]
        with self.assertRaises(TypeError):
            bloom.data.snapshot()['project']['name'] = 'illegal'
        with self.assertRaises(AttributeError):
            node.id = 99
        bloom.ops.composition.add(name='Other', width=64, height=64, duration=(48, 1))
        bloom.ops.composition.delete(composition=1)
        with self.assertRaises(bloom.StaleObjectError) as error:
            node.resolve()
        self.assertEqual(error.exception.id, node.id)
        self.assertEqual(error.exception.revision, bloom.data.snapshot()['revision'])

    def test_events(self):
        called = []
        thread = threading.get_ident()
        with bloom.events.subscribe(lambda event: called.append((threading.get_ident(), event))):
            bloom.ops.project.set_name(name='event')
            self.assertEqual(called, [])
            self.assertGreater(bloom.events.pump(), 0)
            self.assertTrue(all(identity == thread for identity, event in called))
            self.assertTrue(any(event['kind'] == 'revision' for identity, event in called))

    def test_tasks(self):
        started, release = threading.Event(), threading.Event()
        def work(task):
            task.report_progress(1, 2, 'Computing')
            started.set()
            release.wait(2)
            task.check_cancelled()
        handle = bloom.tasks.submit(work)
        self.assertTrue(started.wait(2))
        self.assertEqual(handle.progress.completed, 1)
        handle.cancel()
        release.set()
        with self.assertRaises(bloom.CancelledError):
            handle.result(3)

    def test_event_overflow(self):
        called = []
        with bloom.events.subscribe(called.append):
            for index in range(520):
                bloom.ops.project.set_name(name=f'Event {index}')
            bloom.events.pump()
        self.assertEqual(called[0]['kind'], 'reset')

    def test_worker_render_and_concurrent_read(self):
        with tempfile.TemporaryDirectory(prefix='bloom-python-worker-') as directory:
            started = threading.Event()
            def render(task):
                started.set()
                return bloom.render.frame(12, out=pathlib.Path(directory) / 'worker.png')
            handle = bloom.tasks.submit(render)
            self.assertTrue(started.wait(2))
            for _ in range(10):
                self.assertEqual(bloom.data.snapshot()['revision'], 0)
            self.assertEqual(handle.result(30).published_frames, 1)

    def test_task_cancels_native_range(self):
        with tempfile.TemporaryDirectory(prefix='bloom-python-cancel-') as directory:
            root = pathlib.Path(directory)
            bloom.ops.composition.add(name='Small', width=4, height=4, duration=(48, 1))
            composition = bloom.data.compositions[-1]
            handle = bloom.tasks.submit(lambda task: bloom.render.range(
                0, 999, composition=composition, out=root / 'cancel.png'))
            # The native range is running once the host tracks it beside the Python task. Wait for
            # that (or for the task to end) rather than against a wall-clock budget; ctest bounds it.
            while len(self.host.tasks()) < 2 and not handle.done:
                time.sleep(0.001)
            self.assertGreaterEqual(len(self.host.tasks()), 2)
            handle.cancel()
            # Ends only if the native range cooperates with its parent Python task.
            concurrent.futures.wait([handle._future])
            self.assertTrue(handle.done, 'Native range cooperates with its parent Python task')
            self.assertLess(len(list(root.glob('cancel.*.png'))), 1000)
            with self.assertRaises(bloom.CancelledError):
                handle.result()

    def test_cancelled_render_raises_cancelled_error(self):
        """A render whose cancellation fires raises bloom.CancelledError, not RuntimeError."""
        with tempfile.TemporaryDirectory(prefix='bloom-python-cancel-') as directory:
            root = pathlib.Path(directory)
            bloom.ops.composition.add(name='Small', width=4, height=4, duration=(48, 1))
            composition = bloom.data.compositions[-1]
            # A task ticket without the task worker's tracer, so the error is the render's own.
            task = bloom.tasks.TaskHandle()
            task._ticket = self.host.track_task('Cancelled render')

            def cancel_once_native_work_runs():
                deadline = time.monotonic() + 10
                while len(self.host.tasks()) < 2 and time.monotonic() < deadline:
                    time.sleep(0.001)
                task._ticket.cancel()

            canceller = threading.Thread(target=cancel_once_native_work_runs)
            token = _state._task.set(task)
            canceller.start()
            try:
                with self.assertRaises(bloom.CancelledError):
                    bloom.render.range(0, 999, composition=composition, out=root / 'cancel.png')
            finally:
                _state._task.reset(token)
                canceller.join(10)
                task._ticket.finish('')
            self.assertLess(len(list(root.glob('cancel.*.png'))), 1000)

    def test_worker_cannot_join_authoring_transaction(self):
        with bloom.transactions.group('Author only'):
            handle = bloom.tasks.submit(lambda task: bloom.ops.project.set_name(name='Wrong thread'))
            with self.assertRaisesRegex(RuntimeError, 'authoring thread'):
                handle.result(3)
            bloom.ops.project.set_name(name='Author')
        self.assertEqual(bloom.context.project.name, 'Author')
        bloom.transactions.undo()
        self.assertEqual(bloom.context.project.name, 'Untitled')

    def test_render_matches_cli(self):
        with tempfile.TemporaryDirectory(prefix='bloom-python-') as directory:
            root = pathlib.Path(directory)
            bloom.ops.layer.add_solid(composition=1, name='Red', color=(1, 0, 0, 1), position=(960, 540))
            self.host.save(str(root / 'scene.bloom'))
            result = bloom.render.frame(12, out=root / 'python.png')
            command = subprocess.run([CLI, 'render', '--project', str(root / 'scene.bloom'),
                                      '--frame', '12', '--preset', 'PngRgba8SrgbV1',
                                      '--out', str(root / 'cli')], capture_output=True, timeout=60)
            self.assertEqual(command.returncode, 0, command.stderr.decode())
            expected = (root / 'cli/frame.png').read_bytes()
            self.assertEqual((root / 'python.png').read_bytes(), expected)
            self.assertEqual(result.files[0].digest, 'sha256:' + hashlib.sha256(expected).hexdigest())
            with self.assertRaises(AttributeError):
                result.published_frames = 2
            # #453: an existing file is replaced only on request, here and in bloom-cli.
            with self.assertRaises(RuntimeError):
                bloom.render.frame(12, out=root / 'cli/frame.png')
            self.assertEqual((root / 'cli/frame.png').read_bytes(), expected)
            arguments = [CLI, 'render', '--project', str(root / 'scene.bloom'), '--frame', '12',
                         '--preset', 'PngRgba8SrgbV1', '--out', str(root / 'python-out')]
            (root / 'python-out').mkdir()
            (root / 'python-out/frame.png').write_bytes(b'kept')
            refused = subprocess.run(arguments, capture_output=True, timeout=60)
            self.assertNotEqual(refused.returncode, 0)
            self.assertEqual((root / 'python-out/frame.png').read_bytes(), b'kept')
            replaced = subprocess.run(arguments + ['--replace'], capture_output=True, timeout=60)
            self.assertEqual(replaced.returncode, 0, replaced.stderr.decode())
            self.assertEqual((root / 'python-out/frame.png').read_bytes(), expected)
            self.assertEqual(bloom.render.frame(12, out=root / 'python.png',
                                                replace=True).files[0].digest,
                             result.files[0].digest)

    def test_render_reports_and_requires_the_device(self):
        """#452: CLI and Python report which device rendered; requiring one fails without it."""
        with tempfile.TemporaryDirectory(prefix='bloom-device-') as directory:
            root = pathlib.Path(directory)
            bloom.ops.layer.add_solid(composition=1, name='Red', color=(1, 0, 0, 1),
                                      position=(960, 540))
            self.host.save(str(root / 'scene.bloom'))
            render = [CLI, 'render', '--project', str(root / 'scene.bloom'), '--frame', '12',
                      '--preset', 'PngRgba8SrgbV1']
            result = bloom.render.frame(12, out=root / 'python.png')
            self.assertEqual(result.gpu_frames + result.cpu_reference_frames, 1)
            # No device: the Vulkan loader is pointed at a driver list that does not exist.
            missing = str(root / 'no-driver.json')
            no_device = dict(os.environ, VK_ICD_FILENAMES=missing, VK_DRIVER_FILES=missing)
            refused = subprocess.run(render + ['--out', str(root / 'refused'), '--require-device'],
                                     env=no_device, capture_output=True, timeout=60)
            self.assertNotEqual(refused.returncode, 0)
            self.assertIn(b'error[bloom-cli.render.device-required]', refused.stderr)
            self.assertFalse((root / 'refused/frame.png').exists())
            cpu = subprocess.run(render + ['--out', str(root / 'cpu')], env=no_device,
                                 capture_output=True, timeout=60)
            self.assertEqual(cpu.returncode, 0, cpu.stderr.decode())
            self.assertIn(b'device gpu-frames=0 cpu-reference-frames=1', cpu.stdout)
            self.assertIn(b'warning[bloom-cli.render.cpu-reference]', cpu.stderr)
            script = root / 'require.py'
            script.write_text(f"""import pathlib
import bloom
bloom.ops.layer.add_solid(name='Red', color=(1, 0, 0, 1))
out = pathlib.Path({str(root)!r})
try:
    bloom.render.frame(0, out=out / 'script-required.png', require_device=True)
except RuntimeError as error:
    print('refused:', error)
assert not (out / 'script-required.png').exists()
cpu = bloom.render.frame(0, out=out / 'script-cpu.png')
assert (cpu.gpu_frames, cpu.cpu_reference_frames) == (0, 1), cpu
print('python require-device refused')
""")
            embedded = subprocess.run([CLI, 'run', str(script)], env=no_device,
                                      capture_output=True, timeout=60)
            self.assertEqual(embedded.returncode, 0, embedded.stderr.decode())
            self.assertIn(b'refused: The render requires a GPU device', embedded.stdout)
            self.assertIn(b'python require-device refused', embedded.stdout)
            if result.gpu_frames == 0:
                return  # No device here either; the device-present half has nothing to run.
            required = bloom.render.frame(12, out=root / 'required.png', require_device=True)
            self.assertEqual((required.gpu_frames, required.cpu_reference_frames), (1, 0))
            gpu = subprocess.run(render + ['--out', str(root / 'gpu'), '--require-device'],
                                 capture_output=True, timeout=60)
            self.assertEqual(gpu.returncode, 0, gpu.stderr.decode())
            self.assertIn(b'device gpu-frames=1 cpu-reference-frames=0', gpu.stdout)
            self.assertEqual((root / 'gpu/frame.png').read_bytes(),
                             (root / 'required.png').read_bytes())


if __name__ == '__main__':
    unittest.main()
