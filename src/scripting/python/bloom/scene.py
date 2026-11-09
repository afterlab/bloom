"""Scene building on top of transactions: declare layers, wiring and timed animation, then commit.

A scene is a plan. Declaring a layer, a link or a key list changes nothing; `commit()` sends the
plan through `bloom.transactions` in two steps, because wiring and animation need the ids of what
the first step created:

1. one transaction adds every layer and node (one undo step), and
2. one transaction sets values, parents, mattes, links and keys using those ids (a second step).

Every edit is an ordinary registry operation, so it uses the same commands, validation and undo
history as any other edit. `Scene.undo()` takes back both steps. Keys are sent as columns
(`times`, `values`, `interpolations`), so a long key list is one operation.

    from bloom import scene

    s = scene.Scene("Title card")
    backdrop = s.solid("Backdrop", (0.04, 0.04, 0.08, 1))
    title = s.text("Title", "Hello", position=(960, 540), size=96)
    title.animate("opacity", [(0, 0.0), (12, 1.0)], ease="ease-in-out")
    title.animate("position", [(0, (960, 600)), (24, (960, 540))])
    title.parent_to(backdrop)
    result = s.commit()          # result.layers == {"Backdrop": <id>, "Title": <id>}
    s.undo()                     # both steps

Times are composition frames by default (layer time for a layer's own parameters); pass
`seconds=True` to `animate` to give seconds instead.
"""
from fractions import Fraction

from . import data, ops, transactions
from .errors import OperationError

__all__ = ["Scene", "SceneResult", "Layer", "Node"]


class SceneResult:
    """What a committed scene made: `layers` and `nodes` by declared name, and `steps`, the number
    of undo steps it used."""

    def __init__(self, layers, nodes, steps):
        self.layers = dict(layers)
        self.nodes = dict(nodes)
        self.steps = steps

    def __repr__(self):
        return f"<SceneResult layers={len(self.layers)} nodes={len(self.nodes)} steps={self.steps}>"


class _Handle:
    def __init__(self, scene, name, index):
        self._scene = scene
        self.name = name
        self._index = index

    def _id(self):
        found = self._scene._ids.get(self._index)
        if found is None:
            raise RuntimeError(f"{self.name!r} has no id until its scene is committed")
        return found

    @property
    def id(self):
        """The stable id, available once the scene is committed."""
        return self._id()

    def __repr__(self):
        return f"<{type(self).__name__} {self.name!r}>"


class Layer(_Handle):
    """A declared layer. Its methods add to the scene's plan and return the layer."""

    def set(self, role, value):
        """Set a parameter (`opacity`, `position`, `scale`, `rotation`, `anchor`, ...) to a
        constant."""
        self._scene._later.append(("set", self, role, value))
        return self

    def animate(self, role, keys, *, ease=None, seconds=False):
        """Key a parameter. `keys` is a list of `(time, value)` or `(time, value, interpolation)`;
        `ease` names the interpolation of every key that does not give its own ("linear", "hold",
        "ease-in-out" or an easing equation name)."""
        self._scene._later.append(("animate", self, role, _keys(keys, ease, seconds)))
        return self

    def parent_to(self, parent):
        """Parent this layer's transform to another declared layer (or None to clear)."""
        self._scene._later.append(("parent", self, parent))
        return self

    def matte_with(self, source, *, channel="alpha", inverted=False):
        """Use another declared layer as this layer's track matte."""
        if channel not in ("alpha", "luma"):
            raise ValueError("channel is 'alpha' or 'luma'")
        self._scene._later.append(("matte", self, source, channel, inverted))
        return self


class Node(_Handle):
    """A declared node of the composition's graph."""

    def set(self, role, value):
        """Set one of the node's parameters to a constant."""
        self._scene._later.append(("node-set", self, role, value))
        return self

    def animate(self, role, keys, *, ease=None, seconds=False):
        """Key one of the node's parameters; see `Layer.animate`."""
        self._scene._later.append(("node-animate", self, role, _keys(keys, ease, seconds)))
        return self

    def feeds(self, target, port="image", *, source_port="image"):
        """Link this node's output to a port of another declared node."""
        self._scene._later.append(("link", self, source_port, target, port))
        return self


def _time(value, seconds):
    if not seconds:
        if isinstance(value, bool) or not isinstance(value, int):
            raise ValueError(f"time {value!r} must be a whole frame (pass seconds=True for "
                             "seconds)")
        return value
    exact = Fraction(value).limit_denominator(1_000_000)
    return (exact.numerator, exact.denominator)


def _keys(keys, ease, seconds):
    rows = []
    for key in keys:
        if not isinstance(key, (tuple, list)) or not 2 <= len(key) <= 3:
            raise ValueError("a key is (time, value) or (time, value, interpolation)")
        rows.append((_time(key[0], seconds), _tuple(key[1]),
                     key[2] if len(key) == 3 else ease))
    return tuple(rows)


def _tuple(value):
    return tuple(value) if isinstance(value, list) else value


class Scene:
    """A plan for a scene in one composition (the active one unless `composition` is given)."""

    def __init__(self, label="Scene", *, composition=None):
        self.label = label
        self._composition = composition
        self._now = []      # step 1 declarations: (kind, handle, call)
        self._later = []    # step 2 declarations
        self._ids = {}      # declaration index -> id
        self._names = {}
        self._steps = 0
        self._committed = False

    # -- declarations -----------------------------------------------------------------------

    def _declare(self, handle_type, name, call):
        if name in self._names:
            raise ValueError(f"{name!r} is already declared in this scene")
        handle = handle_type(self, name, len(self._now))
        self._names[name] = handle
        self._now.append((handle, call))
        return handle

    def _in_composition(self, arguments):
        if self._composition is not None:
            arguments["composition"] = self._composition
        return arguments

    def solid(self, name, color, *, position=None, opacity=None):
        """A full-frame solid layer."""
        arguments = self._in_composition({"name": name, "color": tuple(color)})
        if position is not None:
            arguments["position"] = tuple(position)
        if opacity is not None:
            arguments["opacity"] = opacity
        return self._declare(Layer, name, ("layer", ops.layer.add_solid, arguments))

    def text(self, name, text, *, position=None, size=None, color=None, opacity=None):
        """A text layer."""
        arguments = self._in_composition({"name": name, "text": text})
        if position is not None:
            arguments["position"] = tuple(position)
        if size is not None:
            arguments["size"] = size
        if color is not None:
            arguments["color"] = tuple(color)
        if opacity is not None:
            arguments["opacity"] = opacity
        return self._declare(Layer, name, ("layer", ops.layer.add_text, arguments))

    def shape(self, name, kind, *, position=None, size=None):
        """A shape layer (`kind` as `bloom.ops.layer.add_shape` takes it), named `name`."""
        arguments = self._in_composition({"kind": kind})
        if position is not None:
            arguments["position"] = tuple(position)
        if size is not None:
            arguments["size"] = tuple(size)
        handle = self._declare(Layer, name, ("layer", ops.layer.add_shape, arguments))
        self._later.append(("rename", handle, name))
        return handle

    def node(self, name, node_type, *, position=None):
        """A node of `node_type` (for example `bloom.box-blur`) in the composition's graph."""
        arguments = self._in_composition({"nodeType": node_type})
        if position is not None:
            arguments["position"] = tuple(position)
        return self._declare(Node, name, ("node", ops.node.add, arguments))

    def layer(self, name):
        """A declared layer by name."""
        found = self._names[name]
        if not isinstance(found, Layer):
            raise KeyError(name)
        return found

    # -- commit -----------------------------------------------------------------------------

    def commit(self):
        """Send the plan through transactions and return a `SceneResult`."""
        if self._committed:
            raise RuntimeError("This scene was already committed")
        self._committed = True
        if self._now:
            with transactions.group(f"{self.label}: add") as group:
                for _, (_, call, arguments) in self._now:
                    call(**arguments)
            self._steps += 1
            made = {}
            for output in group.result["outputs"]:
                made.setdefault(output["operation_index"], {}).setdefault(
                    output["name"], output["id"])
            for index, (_, (kind, _, _)) in enumerate(self._now):
                self._ids[index] = made[index][kind]
        calls = self._second_step()
        if calls:
            with transactions.group(f"{self.label}: wire and animate"):
                for call, arguments in calls:
                    call(**arguments)
            self._steps += 1
        return SceneResult({h.name: self._ids[h._index] for h, _ in self._now
                            if isinstance(h, Layer)},
                           {h.name: self._ids[h._index] for h, _ in self._now
                            if isinstance(h, Node)},
                           self._steps)

    def undo(self):
        """Take back everything `commit()` did (one undo per step it used)."""
        for _ in range(self._steps):
            transactions.undo()
        self._steps = 0

    # -- step two ---------------------------------------------------------------------------

    def _layer_reference(self, layer):
        reference = {"layer": layer._id()}
        if self._composition is not None:
            reference["composition"] = int(self._composition)
        return reference

    def _node_parameter(self, node, role):
        composition = self._composition if self._composition is not None \
            else data.snapshot()["compositions"][0]["id"]
        for row in data.snapshot()["compositions"]:
            if row["id"] != int(composition):
                continue
            for candidate in row["nodes"]:
                if candidate["id"] == node._id():
                    if role not in candidate["parameters"]:
                        known = ", ".join(sorted(candidate["parameters"]))
                        raise OperationError([{
                            "code": "bloom.scene.unknown-parameter", "operation_id": "",
                            "argument": role,
                            "message": f"{node.name!r} has no parameter {role!r}; it has {known}"}])
                    return candidate["parameters"][role]
        raise RuntimeError(f"{node.name!r} is not in the composition")

    @staticmethod
    def _key_arguments(keys):
        arguments = {"times": tuple(key[0] for key in keys),
                     "values": tuple(key[1] for key in keys), "replace": True}
        if any(key[2] is not None for key in keys):
            arguments["interpolations"] = tuple(key[2] or "linear" for key in keys)
        return arguments

    def _second_step(self):
        calls = []
        for entry in self._later:
            kind = entry[0]
            if kind == "rename":
                _, layer, name = entry
                calls.append((ops.layer.rename, self._with_layer(layer, name=name)))
            elif kind == "set":
                _, layer, role, value = entry
                calls.append((ops.parameter.set_source,
                              self._with_composition({"parameter": {**self._layer_reference(layer),
                                                                    "role": role},
                                                      "value": _tuple(value)})))
            elif kind == "animate":
                _, layer, role, keys = entry
                calls.append((ops.animation.set_key_columns, self._with_composition(
                    {"parameter": {**self._layer_reference(layer), "role": role},
                     **self._key_arguments(keys)})))
            elif kind == "parent":
                _, layer, parent = entry
                arguments = self._with_layer(layer)
                if parent is not None:
                    arguments["parent"] = parent._id()
                calls.append((ops.layer.set_parent, arguments))
            elif kind == "matte":
                _, layer, source, channel, inverted = entry
                calls.append((ops.layer.set_track_matte,
                              self._with_layer(layer, source=source._id(),
                                               channel=0 if channel == "alpha" else 1,
                                               inverted=inverted)))
            elif kind == "node-set":
                _, node, role, value = entry
                calls.append((ops.parameter.set_source,
                              self._with_composition({"parameter": self._node_parameter(node, role),
                                                      "value": _tuple(value)})))
            elif kind == "node-animate":
                _, node, role, keys = entry
                calls.append((ops.animation.set_key_columns, self._with_composition(
                    {"parameter": self._node_parameter(node, role),
                     **self._key_arguments(keys)})))
            elif kind == "link":
                _, source, source_port, target, port = entry
                calls.append((ops.node.connect, self._with_composition(
                    {"sourceNode": source._id(), "sourcePort": source_port,
                     "targetNode": target._id(), "port": port})))
        return calls

    def _with_composition(self, arguments):
        if self._composition is not None:
            arguments["composition"] = self._composition
        return arguments

    def _with_layer(self, layer, **arguments):
        arguments["layer"] = layer._id()
        return self._with_composition(arguments)
