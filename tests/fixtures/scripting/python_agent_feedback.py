"""The agent-feedback contracts through Python: one time contract and parameter value kinds."""
import os
import sys
import tempfile
import unittest
from fractions import Fraction

sys.path.insert(0, sys.argv.pop(1))
import _bloom
import bloom
from bloom import _state


class AgentFeedback(unittest.TestCase):
    def setUp(self):
        self.host = _bloom.Host()
        self.token = _state.bind(self.host)

    def tearDown(self):
        _state._host.reset(self.token)

    def duration(self):
        row = next(c for c in bloom.data.snapshot()['compositions']
                   if c['id'] == int(bloom.context.composition))
        return Fraction(*row['duration'])

    def test_integer_times_are_frames(self):
        # The default composition runs at 24 fps: 48 frames are two seconds, not 48.
        self.assertTrue(bloom.ops.composition.set_duration(48)['succeeded'])
        self.assertEqual(self.duration(), Fraction(2))
        # An exact pair is seconds.
        self.assertTrue(bloom.ops.composition.set_duration((3, 1))['succeeded'])
        self.assertEqual(self.duration(), Fraction(3))
        # A frame count follows a frame rate changed by the same call.
        self.assertTrue(bloom.ops.composition.set_settings(
            frameRateNumerator=30, frameRateDenominator=1, duration=60)['succeeded'])
        self.assertEqual(self.duration(), Fraction(2))
        # And a rate changed earlier in the same transaction.
        with bloom.transactions.group('Rate then duration'):
            bloom.ops.composition.set_settings(frameRateNumerator=25, frameRateDenominator=1)
            bloom.ops.composition.set_duration(50)
        self.assertEqual(self.duration(), Fraction(2))

    def test_parameter_values_in_their_own_kind(self):
        bloom.ops.layer.add_shape('star')
        row = next(c for c in bloom.data.snapshot()['compositions']
                   if c['id'] == int(bloom.context.composition))
        shape = next(n for n in row['nodes'] if 'points' in n['parameters'])
        layer_output = next(n for n in reversed(row['nodes']) if 'blendMode' in n['parameters'])
        points = shape['parameters']['points']
        fill = shape['parameters']['fillEnabled']
        blend = layer_output['parameters']['blendMode']
        self.assertTrue(bloom.ops.parameter.set_source(parameter=points, value=7)['succeeded'])
        self.assertTrue(bloom.ops.parameter.set_source(parameter=blend, value='Multiply')['succeeded'])
        self.assertTrue(bloom.ops.parameter.set_source(parameter=fill, value=False)['succeeded'])
        with self.assertRaises(bloom.OperationError) as refused:
            bloom.ops.parameter.set_source(parameter=points, value=99)
        self.assertIn('3..64', str(refused.exception))
        self.assertEqual(refused.exception.diagnostics[0]['operation_index'], 0)

    def composition(self):
        return next(c for c in bloom.data.snapshot()['compositions']
                    if c['id'] == int(bloom.context.composition))

    def test_set_key_columns_match_set_keys_rows(self):
        # #604: times, values and interpolations as columns are the same edit as rows.
        bloom.ops.layer.add_solid(name='Wash', color=(1, 0, 0, 1))
        opacity = {'layer': 'Wash', 'role': 'opacity'}
        columns = bloom.ops.animation.set_key_columns(
            opacity, (0, 6, 12), (0.0, 0.5, 1.0), interpolations=('hold',), replace=True)
        self.assertTrue(columns['succeeded'])
        made = sum(o['name'] == 'keyframe' for o in columns['outputs'])
        self.assertGreaterEqual(made, 3)
        bloom.transactions.undo()
        rows = bloom.ops.animation.set_keys(opacity, ((0, 0.0, 'hold'), (6, 0.5), (12, 1.0)),
                                            replace=True)
        self.assertEqual(sum(o['name'] == 'keyframe' for o in rows['outputs']), made)
        # Exact times in seconds work in a column too.
        seconds = bloom.ops.animation.set_key_columns(
            opacity, ((0, 1), (1, 2)), (1.0, 0.0), replace=True)
        self.assertTrue(seconds['succeeded'])
        with self.assertRaises(bloom.OperationError) as refused:
            bloom.ops.animation.set_key_columns(opacity, (0, 6), (0.0,))
        self.assertIn('has 1 entries but times has 2', str(refused.exception))
        with self.assertRaises(bloom.OperationError) as extra:
            bloom.ops.animation.set_key_columns(opacity, (0,), (0.0,), interpolations=('a', 'b'))
        self.assertIn('interpolations', str(extra.exception))

    def test_scene_library_builds_wires_and_animates(self):
        # #602: a scene is declared, then committed through transactions as two undo steps.
        before = len(self.composition()['layers'])
        scene = bloom.scene.Scene('Title card')
        back = scene.solid('Backdrop', (0.1, 0.1, 0.2, 1))
        title = scene.text('Title', 'Hello', position=(100, 100))
        shape = scene.shape('Badge', 'star', position=(50, 50))
        blur = scene.node('Soften', 'bloom.box-blur')
        title.animate('opacity', [(0, 0.0), (12, 1.0)], ease='ease-in-out')
        title.animate('position', [(0, (100, 140)), (24, (100, 100))])
        title.parent_to(back)
        shape.set('opacity', 0.5).matte_with(back)
        with self.assertRaises(RuntimeError):
            title.id
        result = scene.commit()
        self.assertEqual(result.steps, 2)
        self.assertEqual(set(result.layers), {'Backdrop', 'Title', 'Badge'})
        self.assertEqual(set(result.nodes), {'Soften'})
        composition = self.composition()
        self.assertEqual(len(composition['layers']), before + 3)
        by_name = {layer['name']: layer for layer in composition['layers']}
        self.assertEqual(by_name['Title']['parent'], result.layers['Backdrop'])
        self.assertEqual(by_name['Badge']['track_matte']['source'], result.layers['Backdrop'])
        self.assertGreaterEqual(len(composition['curves']), 2)
        self.assertEqual(title.id, result.layers['Title'])
        with self.assertRaises(RuntimeError):
            scene.commit()
        scene.undo()
        self.assertEqual(len(self.composition()['layers']), before)
        with self.assertRaises(ValueError):
            bloom.scene.Scene().solid('A', (0, 0, 0, 1)).animate('opacity', [(0.5, 1.0)])
        duplicate = bloom.scene.Scene()
        duplicate.solid('A', (0, 0, 0, 1))
        with self.assertRaises(ValueError):
            duplicate.solid('A', (1, 1, 1, 1))

    def test_missing_font_file_is_a_compile_warning(self):
        # #442: a text layer whose system font file is gone draws the fallback face; the edit says so.
        added = bloom.ops.layer.add_text(name='T', text='Hi')
        self.assertTrue(added['succeeded'])
        self.assertEqual(len(added['compile_warnings']), 0)
        row = next(c for c in bloom.data.snapshot()['compositions']
                   if c['id'] == int(bloom.context.composition))
        font = next(n['parameters']['font'] for n in row['nodes'] if 'font' in n['parameters'])
        with tempfile.TemporaryDirectory() as directory:
            face = os.path.join(directory, 'Gone.ttf')
            ensured = bloom.ops.asset.ensure_font(
                path=face, digest='3' * 64, family='Gone', style='Regular',
                relinkHint='file://' + face)
            asset = next(o['id'] for o in ensured['outputs'] if o['name'] == 'asset')
            result = bloom.ops.parameter.set_source(parameter=font, value=str(asset))
        self.assertTrue(result['succeeded'], result)
        self.assertEqual(len(result['compile_warnings']), 1, result)
        warning = result['compile_warnings'][0]
        self.assertEqual(warning['code'], 'bloom.runtime.compile.font-asset-unavailable')
        self.assertIn('DejaVu Sans was used', warning['detail'])
        self.assertEqual(warning['subject']['parameter'], font)
        # An edit that changes nothing reports no warnings.
        again = bloom.ops.parameter.set_source(parameter=font, value=str(asset))
        self.assertEqual(len(again['compile_warnings']), 0)


if __name__ == '__main__':
    unittest.main()
