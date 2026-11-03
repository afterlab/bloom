from dataclasses import dataclass
from os import PathLike
from .data import Proxy
@dataclass(frozen=True)
class RenderFile:
    path: str
    digest: str
@dataclass(frozen=True)
class RenderResult:
    files: tuple[RenderFile, ...]
    published_frames: int
    preservation_report: str
    gpu_frames: int
    cpu_reference_frames: int
def frame(number: int, *, out: str | PathLike[str], composition: int | Proxy | None = ...,
          preset: str = ..., replace: bool = ..., require_device: bool = ...) -> RenderResult: ...
def range(first: int, last: int, *, out: str | PathLike[str],
          composition: int | Proxy | None = ..., preset: str = ...,
          replace: bool = ..., require_device: bool = ...) -> RenderResult: ...
