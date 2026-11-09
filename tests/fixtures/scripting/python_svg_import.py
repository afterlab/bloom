"""#343: a real script imports an SVG through the Bloom API, moves one point of a compound shape,
keys it, and reads the result back.

Run with `bloom-cli run tests/fixtures/scripting/python_svg_import.py`.
"""
import collections.abc
import pathlib
import tempfile

import bloom



def plain(value):
    """Reads come back as read-only mappings and tuples; edits are written as plain data."""
    if isinstance(value, collections.abc.Mapping):
        return {key: plain(item) for key, item in value.items()}
    if isinstance(value, (tuple, list)):
        return [plain(item) for item in value] if value and isinstance(value[0], collections.abc.Mapping) \
            else tuple(value)
    return value


SVG = """<svg xmlns="http://www.w3.org/2000/svg" width="256" height="144" viewBox="0 0 256 144">
<path id="ring" fill="#336699" fill-rule="evenodd" d="M30 20H110V100H30Z M50 40H90V80H50Z"/>
<rect id="block" x="140" y="30" width="60" height="50" fill="#cc3333"/>
</svg>
"""

with tempfile.TemporaryDirectory(prefix="bloom-svg-import-") as directory:
    file = pathlib.Path(directory) / "art.svg"
    file.write_text(SVG)

    composition = bloom.context.composition
    before = len(bloom.data.nodes(composition))
    result = bloom.ops.layer.add_svg(path=str(file), name="art", fit=False,
                                     projectDirectory=directory)
    assert result["succeeded"], result
    assert {o["name"] for o in result["outputs"]} >= {"layer", "slot", "merge", "asset"}, result
    nodes = bloom.data.nodes(composition)
    print("imported", len(nodes) - before, "nodes")

    # The compound shape is the Path node whose value has two contours.
    parameters = bloom.data.parameters(composition)
    ring = None
    for node in nodes:
        parameter = node.parameters.get("value") if hasattr(node.parameters, "get") else None
        if parameter is None:
            continue
        value = parameters.get(parameter).value
        if isinstance(value, collections.abc.Mapping) and len(value.get("contours", [])) == 2:
            ring = parameter
            break
    assert ring is not None, "the compound shape's Path node"
    rest = plain(parameters.get(ring).value)
    assert rest["fill_rule"] == "evenOdd", rest["fill_rule"]
    print("outer contour corner 1:", rest["contours"][0]["anchors"][1]["point"])
    print("counter corner 1:     ", rest["contours"][1]["anchors"][1]["point"])

    # Move one point of the outer contour and one of the counter, as an artist would drag them.
    moved = plain(rest)
    corner = moved["contours"][0]["anchors"][1]["point"]
    counter = moved["contours"][1]["anchors"][1]["point"]
    moved["contours"][0]["anchors"][1]["point"] = (corner[0] + 20.0, corner[1] - 10.0)
    moved["contours"][1]["anchors"][1]["point"] = (counter[0] - 10.0, counter[1] + 10.0)
    with bloom.transactions.group("Pull the corner and the counter point"):
        first = bloom.ops.animation.set_keyframe_at_time_for_parameter(
            parameter=ring, time=0, value=rest)
        second = bloom.ops.animation.set_keyframe_at_time_for_parameter(
            parameter=ring, time=24, value=moved)
        print("keys:", first, second)

    # Read it back: the parameter is animated and its curve holds the two shapes.
    keyed = bloom.data.parameters(composition).get(ring)
    print("keyed parameter:", keyed)
    curve = next(c for c in bloom.data.curves(composition) if c.resolve()["components"])
    keys = curve.resolve()["components"][0]
    assert len(keys) == 2, keys
    at_start, at_end = keys[0], keys[1]
    assert at_start["time"] == (0, 1), at_start["time"]
    assert at_end["time"] == (1, 1), at_end["time"]
    def point(key, contour):
        return key["value"]["contours"][contour]["anchors"][1]["point"]
    # Frame 0 is the shape as imported; the key at one second holds the two moved points.
    assert point(at_start, 0) == corner and point(at_start, 1) == counter
    assert point(at_end, 0) == (corner[0] + 20.0, corner[1] - 10.0), point(at_end, 0)
    assert point(at_end, 1) == (counter[0] - 10.0, counter[1] + 10.0), point(at_end, 1)
    # The other points did not move.
    assert at_end["value"]["contours"][0]["anchors"][0] == at_start["value"]["contours"][0]["anchors"][0]
    print("keyed:", point(at_start, 0), "->", point(at_end, 0), "and", point(at_start, 1), "->", point(at_end, 1))

    # One undo removes the two keys; the import is a further single undo.
    bloom.transactions.undo()
    bloom.transactions.undo()
    assert len(bloom.data.nodes(composition)) == before, "undoing the import removes every node"
    print("python svg import: PASS")
