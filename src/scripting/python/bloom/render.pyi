from dataclasses import dataclass
from os import PathLike
from typing import Any
from .data import Proxy
@dataclass(frozen=True)
class RenderFile:
    path: str
    digest: str
@dataclass(frozen=True)
class RenderSubtitle:
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
    gpu_frames: int
    cpu_reference_frames: int
    subtitles: tuple[RenderSubtitle, ...]
def frame(number: int, *, out: str | PathLike[str], composition: int | Proxy | None = ...,
          preset: str = ..., replace: bool = ..., require_device: bool = ...,
          subtitles: dict[str, Any] | None = ...) -> RenderResult: ...
def range(first: int, last: int, *, out: str | PathLike[str],
          composition: int | Proxy | None = ..., preset: str = ...,
          replace: bool = ..., require_device: bool = ...,
          subtitles: dict[str, Any] | None = ...) -> RenderResult: ...
