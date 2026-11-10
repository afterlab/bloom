from collections.abc import Iterable
from typing import Any
from .data import Proxy
level: tuple[Proxy, ...]
points: Proxy | None
def set_level(layers: Iterable[Proxy | int], points: Proxy | int | None = None) -> dict[str, Any]: ...
