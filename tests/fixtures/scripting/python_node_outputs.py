"""bloom.data.outputs: a node's outputs at a time, as values (#591).

A Coordinate Space whose origin and pixels per unit are keyed is read by a Space Readout, To Space,
From Space, a Function Plot of y = x, Axes and an Axis Tick. At several frames every readout equals
the keyed map computed here, the conversions are its inverse and the plot's points lie on it; the
answers follow an edit. With the space connected, a keyed Origin on the plot is not read. Bad
arguments raise ValueError.

Usage: python_node_outputs.py <python-dir>
"""
import math
import sys
import unittest
from fractions import Fraction

sys.path.insert(0, sys.argv.pop(1))
import _bloom  # noqa: E402
import bloom  # noqa: E402
from bloom import _state  # noqa: E402

# The space's keys: (frame, origin, pixels per unit), Y up, linear between them.
KEYS = ((0, (256.0, 200.0), (40.0, 40.0)), (24, (216.0, 236.0), (56.0, 44.0)))


def created(result, name):
    assert result['succeeded'], result
    return [output['id'] for output in result['outputs'] if output['name'] == name][0]


def space_at(frame):
    (f0, o0, s0), (f1, o1, s1) = KEYS
    t = min(max((frame - f0) / (f1 - f0), 0.0), 1.0)
    return (tuple(o0[i] + (o1[i] - o0[i]) * t for i in range(2)),
            tuple(s0[i] + (s1[i] - s0[i]) * t for i in range(2)))


def to_pixels(space, point):
    origin, scale = space
    return origin[0] + point[0] * scale[0], origin[1] - point[1] * scale[1]


def values(answer):
    return {output['name']: output['value'] for output in answer['outputs']}


class Outputs(unittest.TestCase):
    def setUp(self):
        self.host = _bloom.Host()
        self.token = _state.bind(self.host)
        self.main = int(bloom.context.composition)
        # y = x as a Function group.
        assert bloom.ops.composition.add(name='Identity', width=64, height=64,
                                         duration=1)['succeeded']
        group = bloom.data.compositions[-1]
        entry = created(bloom.ops.node.add('bloom.group-input', composition=group), 'node')
        leave = created(bloom.ops.node.add('bloom.group-output', composition=group), 'node')
        assert bloom.ops.node.connect(entry, 'scalar1', leave, port='scalar1',
                                      composition=group)['succeeded']
        self.space = self.add('bloom.coordinate-space', name='graph')
        for role, index in (('origin', 1), ('pixelsPerUnit', 2)):
            self.key(self.space, role, [(key[0], list(key[index])) for key in KEYS])
        self.readout = self.add('bloom.space-readout')
        self.to_space = self.add('bloom.to-space', pixels=[300.0, 120.0])
        self.from_space = self.add('bloom.from-space', units=[1.5, 2.5])
        self.plot = self.add('bloom.function-plot', group=int(group), start=-3.0, end=3.0,
                             samples=7, smooth=False)
        self.axes = self.add('bloom.axes')
        self.tick = self.add('bloom.axis-tick', index=3)
        for consumer in (self.readout, self.to_space, self.from_space, self.plot, self.axes,
                         self.tick):
            assert bloom.ops.node.connect(self.space, 'space', consumer, port='space',
                                          composition=self.main)['succeeded']

    def tearDown(self):
        _state._host.reset(self.token)

    def add(self, type_id, **settings):
        node = created(bloom.ops.node.add(type_id, composition=self.main), 'node')
        for role, value in settings.items():
            self.set(node, role, value)
        return node

    def set(self, node, role, value):
        kind = ('boolean' if isinstance(value, bool) else 'integer' if isinstance(value, int)
                else 'number' if isinstance(value, float) else 'text' if isinstance(value, str)
                else 'vector')
        assert bloom.ops.node.set_parameter_value(node, role, composition=self.main,
                                                  **{kind: value})['succeeded'], (role, value)

    def key(self, node, role, keys):
        record = next(item for item in bloom.data.snapshot()['compositions']
                      if item['id'] == self.main)
        parameter = next(item for item in record['nodes'] if item['id'] == node)['parameters'][role]
        assert bloom.ops.animation.create_for_parameter(parameter, time=0,
                                                        composition=self.main)['succeeded']
        for frame, value in keys:
            assert bloom.ops.animation.set_keyframe_at_time_for_parameter(
                parameter, value, time=frame, composition=self.main)['succeeded']
        for curve in bloom.data.curves(self.main):
            ids = tuple((int(curve), item['id']) for component in curve.components
                        for item in component)
            assert bloom.ops.animation.set_keyframes_interpolation(
                keys=ids, interpolation='linear', composition=self.main)['succeeded']

    def assertNear(self, value, expected, tolerance=1e-9, message=None):
        if isinstance(expected, (tuple, list)):
            self.assertEqual(len(value), len(expected), message)
            for got, want in zip(value, expected):
                self.assertLessEqual(abs(got - want), tolerance * max(1.0, abs(want)),
                                     message or (value, expected))
        else:
            self.assertLessEqual(abs(value - expected), tolerance * max(1.0, abs(expected)),
                                 message or (value, expected))

    def test_readouts_follow_the_keyed_space(self):
        for frame in (0, 6, 12, 24, 36):
            space = space_at(frame)
            (ox, oy), (sx, sy) = space
            answer = bloom.data.outputs(self.readout, frame=frame)
            self.assertEqual(answer['status'], 'evaluated')
            self.assertEqual(answer['type'], 'bloom.space-readout')
            self.assertEqual(answer['time']['frames'], frame)
            self.assertEqual(answer['time']['seconds'], (Fraction(frame, 24).numerator,
                                                         Fraction(frame, 24).denominator))
            read = values(answer)
            self.assertNear(read['origin'], (ox, oy))
            self.assertNear(read['xAxis'], (sx, 0.0))
            self.assertNear(read['yAxis'], (0.0, -sy))
            self.assertNear(read['scale'], (sx, sy))
            self.assertNear(read['rotation'], 0.0)
            self.assertNear(read['matrix']['values'], (sx, 0.0, ox, 0.0, -sy, oy, 0.0, 0.0, 1.0))
            self.assertEqual(read['name'], 'graph')
            self.assertEqual(read['parentName'], '')
            self.assertIs(read['projective'], False)
            self.assertIs(read['valid'], True)
            # The space itself, its 4x4 map, and the conversions through it.
            defined = values(bloom.data.outputs(self.space, frame=frame))['space']
            self.assertEqual(defined['name'], 'graph')
            self.assertIs(defined['affine'], True)
            self.assertNear(defined['matrix'][0], (sx, 0.0, 0.0, ox))
            self.assertNear(defined['matrix'][1], (0.0, -sy, 0.0, oy))
            self.assertIsNone(defined['parent'])
            units = values(bloom.data.outputs(self.to_space, frame=frame))['result']
            self.assertNear(units, ((300.0 - ox) / sx, (oy - 120.0) / sy))
            pixels = values(bloom.data.outputs(self.from_space, frame=frame))['result']
            self.assertNear(pixels, to_pixels(space, (1.5, 2.5)))
            # The plot's points of y = x lie on the space's map.
            path = values(bloom.data.outputs(self.plot, frame=frame))['path']
            points = [anchor['point'] for anchor in path['anchors']]
            self.assertEqual(len(points), 7)
            for index, point in enumerate(points):
                self.assertNear(point, to_pixels(space, (index - 3.0, index - 3.0)))
            tick = values(bloom.data.outputs(self.tick, frame=frame))
            self.assertNear(tick['value'], -2.0)
            self.assertNear(tick['position'], (ox - 2.0 * sx, oy + 24.0))
            self.assertEqual(tick['label'], '-2')

    def test_a_time_in_seconds_reads_the_same_frame(self):
        by_frame = bloom.data.outputs(self.readout, frame=12)
        self.assertEqual(bloom.data.outputs(self.readout, time=Fraction(1, 2))['outputs'],
                         by_frame['outputs'])
        self.assertEqual(bloom.data.outputs(self.readout, time=(1, 2))['outputs'],
                         by_frame['outputs'])

    def test_answers_follow_an_edit(self):
        before = bloom.data.outputs(self.to_space, frame=0)
        self.set(self.to_space, 'pixels', [376.0, 40.0])
        after = bloom.data.outputs(self.to_space, frame=0)
        self.assertGreater(after['revision'], before['revision'])
        self.assertNear(values(after)['result'], (3.0, 4.0))
        self.set(self.space, 'name', 'chart')
        self.assertEqual(values(bloom.data.outputs(self.readout))['name'], 'chart')

    def test_placement_is_not_read_while_a_space_is_connected(self):
        plain = values(bloom.data.outputs(self.plot, frame=12))['path']
        tick = values(bloom.data.outputs(self.tick, frame=12))['position']
        # A set origin and a wired pixels per unit are inactive while the space is connected.
        self.set(self.plot, 'origin', [900.0, -300.0])
        self.set(self.tick, 'origin', [900.0, -300.0])
        for consumer in (self.plot, self.axes, self.tick):
            assert bloom.ops.node.connect(self.readout, 'scale', consumer, port='pixelsPerUnit',
                                          composition=self.main)['succeeded']
        answer = bloom.data.outputs(self.plot, frame=12)
        self.assertEqual(answer['status'], 'evaluated')
        self.assertEqual(answer['diagnostics'], ())
        self.assertEqual(values(answer)['path'], plain)
        self.assertEqual(values(bloom.data.outputs(self.tick, frame=12))['position'], tick)
        self.assertEqual(bloom.data.outputs(self.axes, frame=12)['status'], 'evaluated')
        # Disconnected, the tick reads them: its origin and the space's scale at frame 12.
        assert bloom.ops.node.disconnect_input(self.tick, port='space',
                                               composition=self.main)['succeeded']
        (_, _), (sx, _) = space_at(12)
        self.assertNear(values(bloom.data.outputs(self.tick, frame=12))['position'],
                        (900.0 - 2.0 * sx, -300.0 + 24.0))

    def test_bad_arguments_are_refused(self):
        with self.assertRaisesRegex(ValueError, 'does not exist'):
            bloom.data.outputs(99999)
        with self.assertRaisesRegex(ValueError, 'Composition 999 does not exist'):
            bloom.data.outputs(self.readout, composition=999)
        with self.assertRaisesRegex(ValueError, 'Stale revision'):
            bloom.data.outputs(self.readout, expected_revision=1)
        with self.assertRaisesRegex(ValueError, 'frame or time'):
            bloom.data.outputs(self.readout, frame=1, time=0.5)
        current = bloom.data.snapshot()['revision']
        self.assertEqual(bloom.data.outputs(self.readout, expected_revision=current)['revision'],
                         current)
        solid = created(bloom.ops.node.add('bloom.solid-source', composition=self.main), 'node')
        image = bloom.data.outputs(solid)['outputs'][0]
        self.assertEqual((image['kind'], image['value']), ('image', None))
        self.assertIn('Pixels', image['note'])

    def test_the_reference_describes_it(self):
        found, text = _bloom.reference('bloom.data.outputs', False)
        self.assertTrue(found)
        self.assertIn('query:outputs', text)
        self.assertTrue(math.isfinite(len(text)))


if __name__ == '__main__':
    unittest.main()
