"""The desktop Viewer's drill level (#645): the entered groups and the shape in point editing.

Session state, like a double-click in the Viewer: it is never saved and never undone. A headless
session has no Viewer, so setting the level there raises OperationError with "not-live".
"""
from . import _state
from .data import Proxy
from .errors import OperationError


def _layer_id(layer):
    if isinstance(layer, Proxy):
        if layer.kind != "layers":
            raise TypeError(f"expected a layer, not {layer!r}")
        return layer.id
    if isinstance(layer, bool) or not isinstance(layer, int) or layer <= 0:
        raise TypeError("a layer is a layer proxy or a positive layer id")
    return layer


def set_level(layers, points=None):
    """Enter `layers`, group layers from the top, outermost first (an empty sequence is the top
    level), and with `points` edit that shape layer's points. Reads back as bloom.context.level and
    bloom.context.points."""
    level = [_layer_id(layer) for layer in layers]
    shape = None if points is None else _layer_id(points)
    try:
        return _state.host().set_viewer_level(level, shape)
    except RuntimeError as error:
        code, _, message = str(error).partition(": ")
        raise OperationError([{"code": code, "message": message or code}]) from None


def __getattr__(name):
    if name in ("level", "points"):
        from . import context
        return getattr(context, name)
    raise AttributeError(name)
