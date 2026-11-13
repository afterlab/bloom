from collections.abc import Mapping, Sequence
from fractions import Fraction
from os import PathLike
from typing import Any
from .data import Proxy
def read(frame: int | None = ..., *, last_frame: int | None = ...,
         composition: int | Proxy | None = ..., time: Fraction | float | tuple[int, int] | None = ...,
         scopes: Sequence[str] | str | None = ..., signal: str = ..., axis_maximum: int = ...,
         region: Sequence[int] | None = ..., ignore_transparent: bool = ...,
         resolution: str = ..., display: str | None = ..., view: str | None = ...,
         result: str = ..., encoding: str = ...) -> Mapping[str, Any]: ...
def image(out: str | PathLike[str], frame: int | None = ..., *, last_frame: int | None = ...,
          composition: int | Proxy | None = ..., time: Fraction | float | tuple[int, int] | None = ...,
          image: str = ..., width: int | None = ..., height: int | None = ..., signal: str = ...,
          axis_maximum: int = ..., region: Sequence[int] | None = ..., ignore_transparent: bool = ...,
          resolution: str = ..., display: str | None = ..., view: str | None = ...,
          waveform_channels: str = ..., histogram_channels: str = ..., histogram_scale: str = ...,
          brightness: float = ..., replace: bool = ...) -> Mapping[str, Any]: ...
