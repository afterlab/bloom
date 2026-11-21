"""Immutable snapshot projections and stable-ID proxies."""
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from fractions import Fraction

from . import _state
from .errors import StaleObjectError


def snapshot() -> Mapping:
    return _state.freeze(_state.host().snapshot())


_LABELS = {"project": "Project", "compositions": "Composition", "assets": "Asset",
           "layers": "Layer", "nodes": "Node", "parameters": "Parameter", "curves": "Curve"}


def _label(record, kind) -> str:
    """The artist-facing name of a record: its own name, or the leaf of its type."""
    name = record.get("name")
    if not name and kind == "nodes":
        name = record.get("type_id", "").rsplit(".", 1)[-1].replace("-", " ").title()
    if not name and kind == "parameters":
        name = record.get("schema_key", "")
    return name or ""


@dataclass(frozen=True, repr=False)
class Proxy:
    id: int
    kind: str
    composition_id: int | None = None
    _owner: object = None

    def __post_init__(self):
        if self._owner is None:
            object.__setattr__(self, "_owner", _state.host())

    def __repr__(self):
        label = _LABELS.get(self.kind, self.kind.rstrip("s").capitalize())
        try:
            name = _label(self.resolve(), self.kind)
        except StaleObjectError:
            return f"<{label} {self.id} (stale)>"
        return f"<{label} {self.id} {name!r}>" if name else f"<{label} {self.id}>"

    def resolve(self) -> Mapping:
        if self._owner is not _state.host():
            raise StaleObjectError(self.id, _state.host().snapshot()["revision"])
        view = self._owner.snapshot()
        if self.kind == "project":
            records = (view["project"],)
        elif self.kind in ("compositions", "assets"):
            records = view[self.kind]
        else:
            composition = next((item for item in view["compositions"]
                                if item["id"] == self.composition_id), None)
            records = () if composition is None else composition[self.kind]
        for record in records:
            if record["id"] == self.id:
                return _state.freeze(record)
        raise StaleObjectError(self.id, view["revision"])

    def __getattr__(self, name):
        record = self.resolve()
        if name not in record:
            raise AttributeError(name)
        return record[name]

    def __int__(self):
        self.resolve()
        return self.id


class Collection(Sequence):
    def __init__(self, kind, composition=None):
        self.kind = kind
        self.composition = composition

    def _records(self):
        view = snapshot()
        if self.composition is None:
            return view[self.kind]
        identity = int(self.composition)
        for item in view["compositions"]:
            if item["id"] == identity:
                return item[self.kind]
        raise StaleObjectError(identity, view["revision"])

    def __len__(self):
        return len(self._records())

    def __getitem__(self, index):
        rows = self._records()[index]
        if isinstance(index, slice):
            return tuple(self._proxy(item["id"]) for item in rows)
        return self._proxy(rows["id"])

    def _proxy(self, identity):
        return Proxy(identity, self.kind,
                     None if self.composition is None else int(self.composition))

    def get(self, identity: int) -> Proxy:
        proxy = self._proxy(identity)
        proxy.resolve()
        return proxy


compositions = Collection("compositions")
assets = Collection("assets")


def layers(composition) -> Collection:
    return Collection("layers", composition)


def nodes(composition) -> Collection:
    return Collection("nodes", composition)


def parameters(composition) -> Collection:
    return Collection("parameters", composition)


def curves(composition) -> Collection:
    return Collection("curves", composition)


def merge_entries(composition) -> tuple:
    """Every Merge entry of a composition (#502: every entry is a layer), output Merge first and
    each Merge top to bottom. An entry whose layer is not created yet reports layer None,
    layer_kind "none" and the default values it renders with."""
    view = snapshot()
    identity = int(composition)
    for item in view["compositions"]:
        if item["id"] == identity:
            return item["merge_entries"]
    raise StaleObjectError(identity, view["revision"])


def outputs(node, *, composition=None, frame=None, time=None, expected_revision=None) -> Mapping:
    """Every output of `node` at a time, as values: what MCP's `query` kind `outputs` answers.

    The node is evaluated on the CPU reference path; nothing has to be connected to it. `frame` is a
    whole frame of the composition and `time` is seconds (a Fraction, a number or (numerator,
    denominator)); give one or neither (the context's time). The result has `revision`,
    `composition`, `node`, `type`, `time` ({"frames", "seconds"}), `status` ("evaluated" or
    "unavailable", then `reason`), `outputs` (each {"name", "kind", "value"} and a "note" for an
    output with no one value) and `diagnostics`. A space is a dict of `name`, `dimensions`,
    `matrix` (four rows of four: its map from units to composition pixels), `affine`, `range`
    ({"min", "max"} or None) and `parent` (a name or None); "no space" is None. An unknown
    composition or node, a stale `expected_revision` or a bad time raises ValueError.
    """
    from . import context  # context reads Proxy from this module

    if composition is None:
        composition = context.composition
    if composition is None:
        raise ValueError("The current project has no composition")
    composition = int(composition)
    view = _state.host().snapshot()
    if expected_revision is not None and int(expected_revision) != view["revision"]:
        raise ValueError(f"Stale revision: the project is at revision {view['revision']}; read "
                         "again and pass that revision")
    rate = next((item["frame_rate"] for item in view["compositions"] if item["id"] == composition),
                None)
    if rate is None:
        raise ValueError(f"Composition {composition} does not exist; bloom.data.compositions lists "
                         "them")
    if frame is not None and time is not None:
        raise ValueError("Give frame or time, not both; frame is a whole frame number and time is "
                         "seconds (a Fraction, a number or (numerator, denominator))")
    if frame is not None:
        if type(frame) is not int:
            raise ValueError("frame is a whole frame number")
        seconds = Fraction(frame) * rate[1] / rate[0]
    elif time is None:
        seconds = Fraction(context.time)
    elif isinstance(time, tuple):
        if len(time) != 2:
            raise ValueError("time is seconds: a Fraction, a number or (numerator, denominator)")
        seconds = Fraction(time[0], time[1])
    else:
        seconds = Fraction(time)
    answer = _state.host().outputs(composition, int(node), seconds.numerator, seconds.denominator)
    frames = seconds * rate[0] / rate[1]
    answer["time"] = {"frames": int(frames) if frames.denominator == 1 else float(frames),
                      "seconds": (seconds.numerator, seconds.denominator)}
    return _state.freeze(answer)
