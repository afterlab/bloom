"""MCP parity operations from Python: the same registry ops an agent runs over MCP, bound as
bloom.ops.*, each one undoable step."""
import pathlib
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, sys.argv.pop(1))
import _bloom
import bloom
from bloom import _state


def write_png(path, width=8, height=4, rgb=(255, 0, 0)):
    def chunk(kind, data):
        body = kind + data
        return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body))
    raw = (b'\x00' + bytes(rgb) * width) * height
    header = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) +
                     chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
    return path


def outputs(result, name):
    return [output['id'] for output in result['outputs'] if output['name'] == name]


def layer_ids():
    composition = bloom.data.snapshot()['compositions'][0]
    return sorted(layer['id'] for layer in composition['layers'])


class ParityOperations(unittest.TestCase):
    def setUp(self):
        self.host = _bloom.Host()
        self.token = _state.bind(self.host)
        self.directory = tempfile.TemporaryDirectory(prefix='bloom-python-parity-')

    def tearDown(self):
        _state._host.reset(self.token)
        self.directory.cleanup()

    def test_layer_operations(self):
        root = pathlib.Path(self.directory.name)
        still = write_png(root / 'still.png')
        imported = bloom.ops.asset.import_((str(still),), str(root))
        asset = outputs(imported, 'asset')[0]
        added = bloom.ops.layer.add_from_assets(assets=(asset,), time=0)
        layers = outputs(added, 'layer')
        self.assertEqual(layer_ids(), layers)

        copy = outputs(bloom.ops.layer.duplicate(layers=tuple(layers)), 'layer')
        self.assertEqual(layer_ids(), sorted(layers + copy))
        bloom.transactions.undo()
        self.assertEqual(layer_ids(), layers, 'undo removes the copy')
        bloom.transactions.redo()
        self.assertEqual(layer_ids(), sorted(layers + copy), 'redo restores it')

        bloom.ops.layer.delete(layers=tuple(copy))
        self.assertEqual(layer_ids(), layers)
        bloom.transactions.undo()
        self.assertEqual(layer_ids(), sorted(layers + copy), 'undo restores the deleted layer')
        with self.assertRaises(bloom.OperationError):
            bloom.ops.layer.add_from_assets(time=0)

    def test_node_source_from_asset(self):
        root = pathlib.Path(self.directory.name)
        still = write_png(root / 'node.png')
        asset = outputs(bloom.ops.asset.import_((str(still),), str(root)), 'asset')[0]
        composition = bloom.data.snapshot()['compositions'][0]
        before = len(composition['nodes'])
        node = outputs(bloom.ops.node.add_asset_source(asset, position=(40, 0)), 'node')
        self.assertEqual(len(node), 1)
        self.assertEqual(len(bloom.data.snapshot()['compositions'][0]['nodes']), before + 1)
        bloom.transactions.undo()
        self.assertEqual(len(bloom.data.snapshot()['compositions'][0]['nodes']), before)

    def test_project_color_settings(self):
        result = self.host.set_color_settings('aces', 'Linear Rec.2020')
        self.assertEqual((result['code'], result['config'], result['working_space']),
                         ('updated', 'aces', 'Linear Rec.2020'))
        self.assertEqual(result['working_space_label'], 'Linear Rec.2020')
        self.assertEqual(self.host.set_color_settings('aces')['code'], 'no-change')
        with self.assertRaises(RuntimeError):
            self.host.set_color_settings('aces', 'lin_rec709_scene')
        with self.assertRaises(RuntimeError):
            self.host.set_color_settings('srgb')
        neutral = self.host.set_color_settings('neutral')
        self.assertEqual((neutral['working_space'], neutral['working_space_label']),
                         ('lin_rec709_scene', 'Linear Rec.709 (sRGB)'))

    def test_project_color_config_file(self):
        # #680: a configuration file that cannot be read is refused in plain words and changes
        # nothing; "clear" goes back to the built-in configuration as an undoable step.
        root = pathlib.Path(self.directory.name)
        broken = root / 'broken' / 'config.ocio'
        broken.parent.mkdir()
        broken.write_text('this: is: not: [a config\n', encoding='utf-8')
        with self.assertRaises(RuntimeError) as refusal:
            self.host.set_color_settings('file', file=str(broken))
        self.assertTrue(str(refusal.exception).strip())
        with self.assertRaises(RuntimeError):
            self.host.set_color_settings('file')
        aces = self.host.set_color_settings('aces')
        self.assertEqual((aces['code'], aces['config_kind']), ('updated', 'aces'))
        cleared = self.host.set_color_settings('clear')
        self.assertEqual((cleared['code'], cleared['config_kind']), ('updated', 'neutral'))
        bloom.transactions.undo()
        self.assertEqual(self.host.set_color_settings('aces')['code'], 'no-change')

    def test_new_composition_from_assets_and_delete(self):
        root = pathlib.Path(self.directory.name)
        still = write_png(root / 'plate.png', width=64, height=32)
        asset = outputs(bloom.ops.asset.import_((str(still),), str(root)), 'asset')[0]
        created = bloom.ops.composition.add_from_assets((asset,))
        composition = outputs(created, 'composition')[0]
        record = next(c for c in bloom.data.snapshot()['compositions'] if c['id'] == composition)
        self.assertEqual((record['width'], record['height']), (64, 32), 'sized from the footage')
        self.assertEqual(len(record['layers']), 1)
        bloom.transactions.undo()
        self.assertNotIn(composition, [c['id'] for c in bloom.data.snapshot()['compositions']])
        bloom.transactions.redo()
        bloom.ops.asset.delete_items((asset,), compositions=(composition,))
        snapshot = bloom.data.snapshot()
        self.assertNotIn(composition, [c['id'] for c in snapshot['compositions']])
        bloom.transactions.undo()
        self.assertIn(composition, [c['id'] for c in bloom.data.snapshot()['compositions']])

    def test_merge_entries(self):
        """#502: every Merge entry is a layer. A plain entry is edited by merge and slot (its own
        layer created in the same undo step), an Adjustment layer is added, a Layer node is
        inserted and adopts the entry's values, and bloom.data reads it all back."""
        composition = bloom.data.snapshot()['compositions'][0]
        merge = next(node['id'] for node in composition['nodes']
                     if node['type_id'] == 'bloom.layer-stack')
        solid = outputs(bloom.ops.node.add('bloom.solid-source'), 'node')[0]
        slot = outputs(bloom.ops.node.connect(solid, 'image', merge, role='content'),
                       'layerSlot')[0]

        def entry(identity):
            entries = bloom.data.merge_entries(composition['id'])
            return next(item for item in entries if item['slot'] == identity)

        plain = entry(slot)
        self.assertEqual(plain['layer_kind'], 'none')
        self.assertIsNone(plain['layer'])
        self.assertEqual(plain['values']['opacity'], 1.0)
        self.assertEqual(plain['source_node'], solid)

        result = bloom.ops.merge.set_entry_parameter(merge, slot, 'opacity', 0.5)
        layer = outputs(result, 'layer')[0]
        edited = entry(slot)
        self.assertEqual(edited['layer_kind'], 'entry')
        self.assertTrue(edited['entry_layer'])
        self.assertEqual(edited['layer'], layer)
        self.assertEqual(edited['values']['opacity'], 0.5)
        self.assertEqual(edited['pivot'], 'frame')
        record = bloom.data.layers(composition['id']).get(layer)
        self.assertTrue(record.entry_layer)
        self.assertEqual(record.entry_slot, slot)
        self.assertEqual(record.node_id, merge)
        self.assertEqual(record.values['opacity'], 0.5)
        self.assertEqual(record.parameters['opacity'], outputs(result, 'parameter.opacity')[0])
        bloom.transactions.undo()
        self.assertEqual(entry(slot)['layer_kind'], 'none', 'one undo removes edit and layer')
        bloom.transactions.redo()

        bloom.ops.layer.set_solo(solo=True, merge=merge, slot=slot)
        self.assertTrue(entry(slot)['solo'])
        with self.assertRaises(bloom.OperationError):
            bloom.ops.layer.set_shy(True, merge=merge)

        added = bloom.ops.merge.add_adjustment_layer(merge)
        adjustment = outputs(added, 'node')[0]
        adjustment_slot = outputs(added, 'layerSlot')[0]
        top = entry(adjustment_slot)
        self.assertEqual(top['index'], 0)
        self.assertEqual(top['adjustment'], adjustment)
        self.assertEqual(top['name'], 'Adjustment Layer 1')
        bloom.ops.merge.set_entry_parameter(merge, adjustment_slot, 'blendMode', 'screen')
        self.assertNotEqual(entry(adjustment_slot)['values']['blendMode'], 0)

        node = outputs(bloom.ops.merge.insert_layer_node(merge, slot), 'node')[0]
        adopted = entry(slot)
        self.assertEqual(adopted['layer_kind'], 'node')
        self.assertEqual(adopted['layer_node'], node)
        self.assertEqual(adopted['values']['opacity'], 0.5)
        self.assertTrue(adopted['solo'])
        self.assertNotIn(layer, layer_ids(), 'the entry layer left with the adoption')

        ensured = bloom.ops.merge.ensure_entry_layer(merge, slot)
        self.assertEqual(outputs(ensured, 'layer'), [adopted['layer']])

    def test_descriptions_are_published(self):
        schemas = {schema['id']: schema for schema in self.host.schemas()}
        for op in ('bloom.layer.delete', 'bloom.layer.duplicate', 'bloom.layer.add-from-assets',
                   'bloom.node.add-asset-source', 'bloom.merge.ensure-entry-layer',
                   'bloom.merge.insert-layer-node', 'bloom.merge.add-adjustment-layer',
                   'bloom.merge.set-entry-parameter'):
            self.assertIn('Errors:', schemas[op]['description'], op)


if __name__ == '__main__':
    unittest.main()
