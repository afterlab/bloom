"""A graph scene built from Python in one coordinate space stays aligned while the space animates.

Issue #591: a Coordinate Space whose origin and scale are keyed is read by a Function Plot of
y = x and by Axes (each drawn by a stroked Shape entered in the Merge, so path pixels are frame
pixels) and by two Place in Space nodes that position two small red solids. At every frame read, each
dot's centre, a point of the plot and a point of the x axis must be where the space's map puts them,
with nothing drawn just beside the plot and the axis. Renders take the device when one exists (and
must, under require_device); without one the device half reports NotRun and the CPU reference route
checks the same pixels.

Run with `bloom-cli run tests/fixtures/scripting/python_coordinate_space_render.py`: the embedded
interpreter is the scripted route, and the operations are the ones MCP `transact` runs.
"""
import pathlib
import struct
import tempfile
import zlib

import bloom

WIDTH, HEIGHT, DOT = 512, 384, 8
# The space's keys: (frame, origin, pixels per unit), Y up.
KEYS = ((0, (256.0, 200.0), (40.0, 40.0)), (24, (216.0, 236.0), (56.0, 44.0)))
POINT_A, POINT_B = (1.5, 2.5), (-2.5, -1.5)


def created(result, name):
    assert result['succeeded'], result
    return [output['id'] for output in result['outputs'] if output['name'] == name][0]


def nodes(composition):
    return next(c for c in bloom.data.snapshot()['compositions'] if c['id'] == int(composition))['nodes']


def last_node(composition, type_id):
    return [node for node in nodes(composition) if node['type_id'] == type_id][-1]


def read_png(path):
    """Width, height and the RGBA rows of an 8-bit RGBA PNG that is not interlaced."""
    data = pathlib.Path(path).read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', path
    at, packed, width, height = 8, b'', 0, 0
    while at < len(data):
        length, kind = struct.unpack('>I4s', data[at:at + 8])
        body = data[at + 8:at + 8 + length]
        if kind == b'IHDR':
            width, height, depth, color, _, _, interlace = struct.unpack('>IIBBBBB', body)
            assert (depth, color, interlace) == (8, 6, 0), (depth, color, interlace)
        elif kind == b'IDAT':
            packed += body
        at += 12 + length
    raw = zlib.decompress(packed)
    stride = width * 4
    rows, previous = [], bytearray(stride)
    for y in range(height):
        kind = raw[y * (stride + 1)]
        row = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            left = row[x - 4] if x >= 4 else 0
            up = previous[x]
            corner = previous[x - 4] if x >= 4 else 0
            if kind == 1:
                row[x] = (row[x] + left) & 255
            elif kind == 2:
                row[x] = (row[x] + up) & 255
            elif kind == 3:
                row[x] = (row[x] + (left + up) // 2) & 255
            elif kind == 4:
                estimate = left + up - corner
                near = min((abs(estimate - left), 0, left), (abs(estimate - up), 1, up),
                           (abs(estimate - corner), 2, corner))[2]
                row[x] = (row[x] + near) & 255
        rows.append(bytes(row))
        previous = row
    return width, height, rows


def pixel(frame, x, y):
    row = frame[2][int(y)]
    return tuple(row[int(x) * 4:int(x) * 4 + 4])


def space_at(frame):
    """The keyed origin and scale at a frame: linear between the two keys, held after the last."""
    (f0, o0, s0), (f1, o1, s1) = KEYS
    t = min(max((frame - f0) / (f1 - f0), 0.0), 1.0)
    mix = lambda a, b: tuple(a[i] + (b[i] - a[i]) * t for i in range(2))  # noqa: E731
    return mix(o0, o1), mix(s0, s1)


def to_pixels(space, point):
    origin, scale = space
    return origin[0] + point[0] * scale[0], origin[1] - point[1] * scale[1]


def red_centroid(frame, about):
    """The coverage-weighted centre of the red pixels in a box about a point. Colour is stored
    unpremultiplied, so a red pixel's alpha is its coverage."""
    total = sx = sy = 0.0
    for y in range(int(about[1]) - 12, int(about[1]) + 13):
        for x in range(int(about[0]) - 12, int(about[0]) + 13):
            r, g, b, a = pixel(frame, x, y)
            if a > 0 and r > 128 and g < 64 and b < 64:
                total += a
                sx += a * (x + 0.5)
                sy += a * (y + 0.5)
    assert total > 0, 'no red pixels about %r' % (about,)
    return sx / total, sy / total


def build():
    assert bloom.ops.composition.add(name='Graph', width=WIDTH, height=HEIGHT, duration=(4, 1),
                                     background=(0, 0, 0, 0))['succeeded']
    main = bloom.data.compositions[-1]
    # y = x as a Function group: Group Input's x straight to Group Output.
    assert bloom.ops.composition.add(name='Identity', width=64, height=64, duration=1)['succeeded']
    group = bloom.data.compositions[-1]
    entry = created(bloom.ops.node.add('bloom.group-input', composition=group), 'node')
    leave = created(bloom.ops.node.add('bloom.group-output', composition=group), 'node')
    assert bloom.ops.node.connect(entry, 'scalar1', leave, port='scalar1',
                                  composition=group)['succeeded']

    def add(type_id, **values):
        node = created(bloom.ops.node.add(type_id, composition=main), 'node')
        for role, value in values.items():
            kind = ('boolean' if isinstance(value, bool) else 'integer' if isinstance(value, int)
                    else 'number' if isinstance(value, float) else 'text' if isinstance(value, str)
                    else 'vector')
            assert bloom.ops.node.set_parameter_value(node, role, composition=main,
                                                      **{kind: value})['succeeded'], (type_id, role)
        return node

    def connect(source, output, target, port):
        assert bloom.ops.node.connect(source, output, target, port=port,
                                      composition=main)['succeeded'], (output, port)

    # Two small red dots, each a layer whose Position a Place in Space drives.
    dots = []
    for name in ('Dot A', 'Dot B'):
        assert bloom.ops.layer.add_solid(name, (1, 0, 0, 1), (0, 0), composition=main)['succeeded']
        solid = last_node(main, 'bloom.solid-source')['id']
        for role in ('width', 'height'):
            assert bloom.ops.node.set_parameter_value(solid, role, number=float(DOT),
                                                      composition=main)['succeeded']
        dots.append(last_node(main, 'bloom.layer-output')['id'])
    merge = last_node(main, 'bloom.layer-stack')['id']

    space = add('bloom.coordinate-space', name='graph')
    parameters = last_node(main, 'bloom.coordinate-space')['parameters']
    for role, index in (('origin', 1), ('pixelsPerUnit', 2)):
        parameter = parameters[role]
        assert bloom.ops.animation.create_for_parameter(parameter, time=0,
                                                        composition=main)['succeeded']
        for key in KEYS:
            assert bloom.ops.animation.set_keyframe_at_time_for_parameter(
                parameter, list(key[index]), time=key[0], composition=main)['succeeded']
    for curve in bloom.data.curves(main):
        keys = tuple((int(curve), key['id']) for component in curve.components for key in component)
        assert bloom.ops.animation.set_keyframes_interpolation(
            keys=keys, interpolation='linear', composition=main)['succeeded']

    plot = add('bloom.function-plot', group=int(group), start=-3.0, end=3.0, samples=7, smooth=False)
    axes = add('bloom.axes', xMin=-4.0, xMax=4.0, yMin=-3.0, yMax=3.0, tickLength=10.0)
    for consumer in (plot, axes):
        connect(space, 'space', consumer, 'space')
    for driver, color, width in ((plot, (0, 1, 0, 1), 3.0), (axes, (0, 0, 1, 1), 2.0)):
        shape = add('bloom.shape-source', kind=6, fillEnabled=False, strokeEnabled=True,
                    strokeColor=list(color), strokeWidth=width)
        connect(driver, 'path', shape, 'path')
        assert bloom.ops.node.connect(shape, 'image', merge, role='content',
                                      composition=main)['succeeded']
    for dot, point in zip(dots, (POINT_A, POINT_B)):
        place = add('bloom.place-in-space', point=list(point))
        connect(space, 'space', place, 'space')
        connect(place, 'position', dot, 'position')
    return main


with tempfile.TemporaryDirectory(prefix='bloom-coordinate-space-') as directory:
    root = pathlib.Path(directory)
    composition = build()
    device = None
    for number in (0, 6, 12, 24, 36):
        out = root / ('frame-%d.png' % number)
        result = bloom.render.frame(number, composition=composition, out=out)
        if device is None:
            device = result.gpu_frames == 1
            if not device:
                print('NotRun: no GPU device here; the scene renders on the CPU reference only')
        assert (result.gpu_frames, result.cpu_reference_frames) == ((1, 0) if device else (0, 1)), \
            'every frame takes the same route: %r' % (result,)
        frame = read_png(out)
        space = space_at(number)
        for point in (POINT_A, POINT_B):
            expected = to_pixels(space, point)
            found = red_centroid(frame, expected)
            assert abs(found[0] - expected[0]) <= 0.02 and abs(found[1] - expected[1]) <= 0.02, \
                'frame %d: the dot at %r is at %r, not %r' % (number, point, found, expected)
        on_plot = to_pixels(space, (-1.5, -1.5))
        found = pixel(frame, *on_plot)
        assert found[1] > 128 and found[3] > 128, \
            'frame %d: the plot passes %r (%r)' % (number, on_plot, found)
        assert pixel(frame, on_plot[0], on_plot[1] - 12)[3] == 0, \
            'frame %d: nothing is drawn just above the plot' % number
        on_axis = to_pixels(space, (2.5, 0.0))
        found = pixel(frame, *on_axis)
        assert found[2] > 128 and found[3] > 128, \
            'frame %d: the x axis passes %r (%r)' % (number, on_axis, found)
        assert pixel(frame, on_axis[0], on_axis[1] - 8)[3] == 0, \
            'frame %d: nothing is drawn just above the x axis' % number
    if device:
        required = bloom.render.frame(12, composition=composition, out=root / 'required.png',
                                      require_device=True)
        assert (required.gpu_frames, required.cpu_reference_frames) == (1, 0), required
    print('coordinate space render: device=%s, five frames aligned' % device)
