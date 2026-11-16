from collections.abc import Mapping, Sequence
from fractions import Fraction
from os import PathLike
from typing import Any
from .data import Proxy

# A read returns a frozen mapping (see bloom.scopes). Each Display frame has "frame_counters"
# ({"under": [r, g, b, luma|None], "over": [r, g, b, luma|None], "not_finite": int}: what the
# display encoding left outside 0..1 in the whole frame; the tables' own under and over are 0) and
# "stored_layout" ("10:10:10:2" or "8:8:8:8"). A Working frame has neither.
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
          replace: bool = ...) -> Mapping[str, Any]: ...
