"""bloom.help(): the reference the MCP help tool and bloom-cli help print, entry for entry."""
import json as _json

import _bloom


def help(name=None, *, json=False):
    """Print the reference index, or one entry in full.

    `name` is an operation id (bloom.layer.add-solid, or its Python spelling
    bloom.ops.layer.add_solid), an MCP tool (query, transact), a query kind (query:layers), a
    bloom-cli command (bloom-cli render) or a concept (concept:time). With json=True the entry is
    returned as a dict instead of printed. An unknown name prints the closest entries.
    """
    if name is not None and not isinstance(name, str):
        name = getattr(name, "id", None) or str(name)
    found, text = _bloom.reference(name or "", bool(json))
    if json:
        return _json.loads(text)
    print(text, end="")
    return None
