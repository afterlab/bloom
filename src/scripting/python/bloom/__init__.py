"""Bloom's versioned artist API. All durable edits use the host command boundary."""
from . import app, context, data, events, ops, render, scene, tasks, transactions
from ._help import help
from .errors import CancelledError, OperationError, StaleObjectError

__all__ = ["app", "context", "data", "events", "ops", "render", "scene", "tasks", "transactions",
           "CancelledError", "OperationError", "StaleObjectError", "help"]


def __getattr__(name):
    if name in ("ui", "addons", "props", "types"):
        raise NotImplementedError(f"bloom.{name} is deferred in v1; see ADR 0022 and ADR 0012")
    raise AttributeError(name)
