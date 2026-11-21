"""Reference frame publication through the host render runner."""
from dataclasses import dataclass
from pathlib import Path

from . import _state, context
from .errors import CancelledError


@dataclass(frozen=True)
class RenderFile:
    path: str
    digest: str


@dataclass(frozen=True)
class RenderSubtitle:
    """One subtitle track a render delivered (#752): a sidecar file, with its size and SHA-256."""
    node: int
    language: str
    title: str
    format: str
    path: str
    cues: int
    bytes: int
    sha256: str


@dataclass(frozen=True)
class RenderResult:
    files: tuple[RenderFile, ...]
    published_frames: int
    preservation_report: str
    # Frames the GPU evaluated and frames the CPU reference path evaluated (#452).
    gpu_frames: int
    cpu_reference_frames: int
    # The subtitle tracks delivered, as the MCP result's `subtitles` lists them (#752).
    subtitles: tuple[RenderSubtitle, ...] = ()


_SUBTITLE_FORMATS = ("srt", "vtt", "ttml")


def _subtitles(value):
    """Checks a subtitles dict {"sidecar": [...], "embed": bool, "nodes": [...]} and returns it
    with sidecars and nodes ascending, or None when no subtitles were asked for."""
    if value is None:
        return None
    if not isinstance(value, dict):
        raise ValueError("subtitles is a dict: {'sidecar': ['srt', 'vtt', 'ttml'], 'embed': False, "
                         "'nodes': [12]}")
    unknown = sorted(set(value) - {"sidecar", "embed", "nodes"})
    if unknown:
        raise ValueError("Unknown subtitles key: " + ", ".join(str(key) for key in unknown))
    sidecar = value.get("sidecar", [])
    if isinstance(sidecar, str) or not isinstance(sidecar, (list, tuple)):
        raise ValueError("subtitles['sidecar'] is a list of srt, vtt and ttml")
    for item in sidecar:
        if item not in _SUBTITLE_FORMATS:
            raise ValueError("subtitles['sidecar'] formats are srt, vtt and ttml")
    if len(set(sidecar)) != len(sidecar):
        raise ValueError("subtitles['sidecar'] names each format at most once")
    embed = value.get("embed", False)
    if type(embed) is not bool:
        raise ValueError("subtitles['embed'] is a boolean")
    if not sidecar and not embed:
        raise ValueError("subtitles needs a sidecar format (srt, vtt or ttml) or embed=True")
    nodes = value.get("nodes")
    if nodes is not None:
        if isinstance(nodes, (str, bytes)) or not isinstance(nodes, (list, tuple)):
            raise ValueError("subtitles['nodes'] is a list of Subtitles node ids")
        if not 1 <= len(nodes) <= 256:
            raise ValueError("subtitles['nodes'] lists 1 to 256 nodes; leave it out for every "
                             "Subtitles node with Export as track on")
        if any(type(node) is not int or node < 1 for node in nodes):
            raise ValueError("subtitles['nodes'] are positive integer node ids")
        if len(set(nodes)) != len(nodes):
            raise ValueError("subtitles['nodes'] names each node at most once")
        nodes = sorted(nodes)
    order = {name: position for position, name in enumerate(_SUBTITLE_FORMATS)}
    return {"sidecar": sorted(sidecar, key=order.__getitem__), "embed": embed, "nodes": nodes}


def _render(first, last, is_range, out, composition, preset, replace, require_device,
            subtitles=None):
    if type(first) is not int or type(last) is not int or first < 0 or last < first:
        raise ValueError("Frames must be nonnegative integers in ascending order")
    if composition is None:
        composition = context.composition
    if composition is None:
        raise ValueError("The current project has no composition")
    subtitles = _subtitles(subtitles)
    task = _state._task.get()
    cancellation_task = task._ticket.id if task is not None and task._ticket is not None else 0
    result = _state.host().render(int(composition), first, last, is_range, preset, str(Path(out)),
                                 cancellation_task, bool(replace), bool(require_device), subtitles)
    if not result["succeeded"]:
        if result["diagnostic_code"] == "cancelled":
            raise CancelledError(result["diagnostic"])
        raise RuntimeError(result["diagnostic"])
    return RenderResult(tuple(RenderFile(**item) for item in result["files"]),
                        result["published_frames"], result["preservation_report"],
                        result["gpu_frames"], result["cpu_reference_frames"],
                        tuple(RenderSubtitle(**item) for item in result["subtitles"]))


def frame(number: int, *, out, composition=None, preset="PngRgba8SrgbV1",
          replace=False, require_device=False, subtitles=None) -> RenderResult:
    return _render(number, number, False, out, composition, preset, replace, require_device,
                   subtitles)


def range(first: int, last: int, *, out, composition=None, preset="PngRgba8SrgbV1",
          replace=False, require_device=False, subtitles=None) -> RenderResult:
    return _render(first, last, True, out, composition, preset, replace, require_device,
                   subtitles)
