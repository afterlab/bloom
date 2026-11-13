"""bloom.scopes: the Scopes panel's tables, a summary and a picture without a panel (#724)."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, sys.argv.pop(1))
import _bloom
import bloom
from bloom import _state


def nonzero_cells(table):
    """(index, count) of a dense table's non-zero cells."""
    return [(i, v) for i, v in enumerate(table['values']) if v]


class Scopes(unittest.TestCase):
    def setUp(self):
        self.host = _bloom.Host()
        self.token = _state.bind(self.host)
        # One composition-sized solid: every pixel has the same color.
        bloom.ops.layer.add_solid(name='Card', color=(0.25, 0.5, 0.75, 1))

    def tearDown(self):
        _state._host.reset(self.token)

    def test_tables_of_a_flat_color(self):
        answer = bloom.scopes.read(0, signal='working', axis_maximum=2, resolution='quarter',
                                   scopes=['histogram'])
        frame = answer['frames'][0]
        self.assertEqual(len(answer['frames']), 1)
        self.assertEqual(frame['frame'], 0)
        self.assertIn(frame['route'], ('cpu', 'gpu'))
        self.assertFalse(frame['from_cache'])
        self.assertGreater(frame['counted'], 0)
        self.assertEqual(answer['luma']['kr'], answer['luma']['kr'])
        self.assertNotIn('waveform', frame['tables'])
        histogram = frame['tables']['histogram']
        # Channels are red, green, blue and luma; a flat color puts all of a channel's pixels into
        # one bin, and the bins rise with the color's channel value.
        bins = histogram['shape'][1]
        per_channel = []
        for channel in range(3):
            row = histogram['values'][channel * bins:(channel + 1) * bins]
            cells = [(i, v) for i, v in enumerate(row) if v]
            self.assertEqual(len(cells), 1, cells)
            self.assertEqual(cells[0][1], frame['counted'])
            per_channel.append(cells[0][0])
        self.assertLess(per_channel[0], per_channel[1])
        self.assertLess(per_channel[1], per_channel[2])

    def test_sparse_is_the_dense_tables_non_zero_cells(self):
        dense = bloom.scopes.read(0, signal='working', resolution='quarter',
                                  scopes=['vectorscope'])['frames'][0]['tables']['vectorscope']
        sparse = bloom.scopes.read(0, signal='working', resolution='quarter',
                                   scopes=['vectorscope'],
                                   encoding='sparse')['frames'][0]['tables']['vectorscope']
        self.assertEqual(sparse['encoding'], 'sparse')
        pairs = sparse['values']
        rebuilt = [(pairs[i], pairs[i + 1]) for i in range(0, len(pairs), 2)]
        self.assertEqual(rebuilt, nonzero_cells(dense))

    def test_summary_range_and_time(self):
        answer = bloom.scopes.read(0, last_frame=2, result='summary', signal='working',
                                   resolution='quarter')
        self.assertEqual([f['frame'] for f in answer['frames']], [0, 1, 2])
        red = answer['frames'][0]['summary']['channels']['red']
        self.assertEqual(red['min'], red['max'])
        by_time = bloom.scopes.read(time=(1, 24), result='summary', signal='working',
                                    resolution='quarter')
        self.assertEqual(by_time['frames'][0]['frame'], 1)

    def test_image_is_written_and_replaced_only_when_asked(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, 'scope.png')
            answer = bloom.scopes.image(path, 0, image='waveform', width=256, height=192,
                                        resolution='quarter')
            picture = answer['frames'][0]['image']
            self.assertEqual((picture['width'], picture['height']), (256, 192))
            with open(path, 'rb') as file:
                self.assertEqual(file.read(8), b'\x89PNG\r\n\x1a\n')
            with self.assertRaises(Exception) as refused:
                bloom.scopes.image(path, 0, image='waveform', resolution='quarter')
            self.assertIn('replace', str(refused.exception))
            bloom.scopes.image(path, 0, image='waveform', resolution='quarter', replace=True)

    def test_bad_arguments_say_what_to_use(self):
        with self.assertRaises(ValueError) as both:
            bloom.scopes.read(0, time=0)
        self.assertIn('frame or time', str(both.exception))
        with self.assertRaises(Exception) as scope:
            bloom.scopes.read(0, scopes=['scope'])
        self.assertIn('chromaticity', str(scope.exception))
        with self.assertRaises(Exception) as axis:
            bloom.scopes.read(0, signal='working', axis_maximum=3)
        self.assertIn('1, 2, 4, 8 or 16', str(axis.exception))
        with self.assertRaises(Exception) as width:
            bloom.scopes.image(os.path.join(tempfile.gettempdir(), 'x.png'), 0, width=9999)
        self.assertIn('128 to 2048', str(width.exception))


if __name__ == '__main__':
    unittest.main()
