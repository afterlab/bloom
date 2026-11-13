"""A scripted render through Apply LUT (`bloom.ocio-file-transform`) runs on the GPU and matches the CPU reference.

Issue #608: `bloom.render.frame` on a composition with an active LUT took the CPU reference path
while the same composition without the LUT rendered on the device. A red solid goes through a
File Transform holding a 2-point 3D `.cube` that rotates the channels (red comes out blue). With
the LUT selected, bypassed and cleared, the render must be evaluated on the device when one
exists, and the LUT frame must match bloom-cli's CPU reference render of the saved project within
one 8-bit code. Without a device the device half reports NotRun and nothing else is skipped.

Run with `bloom-cli run tests/fixtures/scripting/python_lut_render.py`: the embedded interpreter is
the scripted route, and its renders use the packaged shader tools of the bloom-cli that runs it.
"""
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

import bloom

# The embedded interpreter's executable is the bloom-cli running this script.
CLI = sys.executable


# The scene, as source: this process runs it, and so does the CPU reference process below. It
# expects `root` (a folder) and leaves `composition`, `transform` and `lut`.
SCENE = """
import bloom

def created(result, name):
    assert result['succeeded'], result
    return [output['id'] for output in result['outputs'] if output['name'] == name]

cube = root / 'rotate.cube'
# Red is the fastest index. Each entry is the input with its channels rotated.
cube.write_text('LUT_3D_SIZE 2\\n' + ''.join(
    '%d %d %d\\n' % (g, b, r) for b in (0, 1) for g in (0, 1) for r in (0, 1)))
assert bloom.ops.composition.add(name='Small', width=64, height=48,
                                 duration=(48, 1))['succeeded']
composition = bloom.data.compositions[-1]
lut = created(bloom.ops.asset.import_((str(cube),), str(root)), 'asset')[0]
merge = next(node['id'] for node in bloom.data.snapshot()['compositions'][-1]['nodes']
             if node['type_id'] == 'bloom.layer-stack')
solid = created(bloom.ops.node.add('bloom.solid-source', composition=composition), 'node')[0]
assert bloom.ops.node.set_parameter_value(solid, 'color', vector=(1, 0, 0, 1),
                                          composition=composition)['succeeded']
transform = created(bloom.ops.node.add('bloom.ocio-file-transform', composition=composition),
                    'node')[0]
assert bloom.ops.node.connect(solid, 'image', transform, port='input',
                              composition=composition)['succeeded']
assert bloom.ops.node.connect(transform, 'image', merge, role='content',
                              composition=composition)['succeeded']
"""


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


def widest_difference(first, second):
    """The largest per-channel difference between two frames of one size."""
    assert first[:2] == second[:2], (first[:2], second[:2])
    return max(abs(a - b) for one, two in zip(first[2], second[2]) for a, b in zip(one, two))


with tempfile.TemporaryDirectory(prefix='bloom-lut-render-') as directory:
    root = pathlib.Path(directory)
    scene = {'root': root}
    exec(SCENE, scene)
    composition, transform, lut = scene['composition'], scene['transform'], scene['lut']

    def render(name, **options):
        out = root / (name + '.png')
        result = bloom.render.frame(0, composition=composition, out=out, **options)
        return result, read_png(out)

    def middle(frame):
        return tuple(frame[2][24][32 * 4:32 * 4 + 4])

    def route(result):
        return (result.gpu_frames, result.cpu_reference_frames)

    # No LUT selected: the solid as authored.
    plain, plain_frame = render('plain')
    assert middle(plain_frame) == (255, 0, 0, 255), middle(plain_frame)
    device = plain.gpu_frames == 1
    expected = (1, 0) if device else (0, 1)
    if not device:
        print('NotRun: no GPU device here; the LUT renders on the CPU reference only')

    assert bloom.ops.node.set_parameter_value(transform, 'lut', integer=lut,
                                              composition=composition)['succeeded']
    selected, selected_frame = render('selected')
    assert middle(selected_frame) == (0, 0, 255, 255), middle(selected_frame)
    assert route(selected) == expected, \
        'a File Transform render is evaluated on the device when one exists: %r' % (selected,)
    if device:
        required, _ = render('required', require_device=True)
        assert route(required) == (1, 0), required

    # The CPU reference: the same script state rendered by a bloom-cli that finds no Vulkan
    # driver. The frame it writes is the one this process compares its device frame to.
    reference_script = root / 'reference.py'
    reference_folder = root / 'reference'
    reference_folder.mkdir()
    reference_script.write_text("""import pathlib
root = pathlib.Path(%r)
""" % str(reference_folder) + SCENE + """
assert bloom.ops.node.set_parameter_value(transform, 'lut', integer=lut,
                                          composition=composition)['succeeded']
result = bloom.render.frame(0, composition=composition, out=root / 'reference.png')
assert (result.gpu_frames, result.cpu_reference_frames) == (0, 1), result
""")
    missing = str(root / 'no-driver.json')
    reference = subprocess.run(
        [CLI, 'run', str(reference_script)],
        env=dict(os.environ, VK_ICD_FILENAMES=missing, VK_DRIVER_FILES=missing),
        capture_output=True, timeout=120)
    assert reference.returncode == 0, reference.stderr.decode() + reference.stdout.decode()
    difference = widest_difference(selected_frame, read_png(reference_folder / 'reference.png'))
    assert difference <= 1, \
        'the device frame matches the CPU reference within one code, not %d' % difference

    # Bypass on: the solid again, still on the device.
    assert bloom.ops.node.set_parameter_value(transform, 'bypass', boolean=True,
                                              composition=composition)['succeeded']
    bypassed, bypassed_frame = render('bypassed')
    assert middle(bypassed_frame) == (255, 0, 0, 255), middle(bypassed_frame)
    assert route(bypassed) == expected, bypassed
    print('lut render: device=%s selected=%r difference=%d' % (device, route(selected), difference))
