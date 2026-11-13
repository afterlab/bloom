"""Scopes without a panel: the Scopes panel's count tables, a summary of them, or a picture.

    import bloom

    summary = bloom.scopes.read(24, result="summary")
    summary["frames"][0]["summary"]["channels"]["luma"]["mean"]
    bloom.scopes.image("scopes.png", 24, image="all")

A read measures the frames exactly as the Scopes panel does (the same definitions, the same GPU and
CPU routes) and returns a dict with the members of the MCP `query` kind `scopes`, in snake_case:
`settings`, `luma`, `primaries` and `frames`. Every frame reports its `route` ("gpu" or "cpu", with
`route_note` saying why the CPU reference counted a frame the device could not), `counted`,
`measured_area` and its `tables` or `summary`. The Display signal goes through the color
configuration's default display and view unless `display` and `view` name another; an open
Viewer's exposure, gamma, channel or isolation never reach a read.

Inside `bloom.tasks.submit` a read reports its progress per frame and stops when the task is
cancelled (CancelledError). A bad argument raises ValueError saying what to use instead.
"""
from fractions import Fraction
from pathlib import Path

from . import _state, context
from .data import Proxy
from .errors import CancelledError

__all__ = ["read", "image"]


def _frame_of(composition, frame, time):
    """The first frame of a read: `frame`, else the frame nearest `time`, else the context's."""
    if frame is not None and time is not None:
        raise ValueError("Give frame or time, not both; frame is a whole frame number and time is "
                         "seconds (a Fraction, a number or (numerator, denominator))")
    if frame is not None:
        if type(frame) is not int or frame < 0:
            raise ValueError("frame is a nonnegative whole frame number")
        return frame
    if time is None:
        time = context.time
    elif isinstance(time, tuple):
        if len(time) != 2:
            raise ValueError("time is seconds: a Fraction, a number or (numerator, denominator)")
        time = Fraction(time[0], time[1])
    else:
        time = Fraction(time)
    rate = _state.host().snapshot()
    for item in rate["compositions"]:
        if item["id"] == composition:
            numerator, denominator = item["frame_rate"]
            exact = time * numerator / denominator
            return max(0, int((exact + Fraction(1, 2)).__floor__()))
    raise ValueError(f"Composition {composition} does not exist; bloom.data.compositions lists "
                     "them")


def _request(frame, last_frame, composition, time, scopes, signal, axis_maximum, region,
             ignore_transparent, resolution, display, view, result, encoding, image, width,
             height, waveform_channels, histogram_channels, histogram_scale, brightness, out,
             replace):
    if composition is None:
        composition = context.composition
    if composition is None:
        raise ValueError("The current project has no composition")
    composition = int(composition)
    first = _frame_of(composition, frame, time)
    if last_frame is None:
        last_frame = first
    if type(last_frame) is not int or last_frame < 0:
        raise ValueError("last_frame is a nonnegative whole frame number")
    if scopes is not None:
        if isinstance(scopes, str):
            scopes = [scopes]
        scopes = [str(name) for name in scopes]
    return {
        "composition": composition, "first_frame": first, "last_frame": last_frame,
        "scopes": scopes, "signal": signal, "axis_maximum": axis_maximum,
        "region": None if region is None else [int(value) for value in region],
        "ignore_transparent": bool(ignore_transparent), "resolution": resolution,
        "display": display or "", "view": view or "", "result": result, "encoding": encoding,
        "image": image, "width": 0 if width is None else int(width),
        "height": 0 if height is None else int(height),
        "waveform_channels": waveform_channels, "histogram_channels": histogram_channels,
        "histogram_scale": histogram_scale, "brightness": float(brightness),
        "out": None if out is None else str(Path(out)), "replace": bool(replace),
    }


def _run(request):
    task = _state._task.get()
    cancellation_task = task._ticket.id if task is not None and task._ticket is not None else 0

    def progress(done, total):
        if task is not None:
            task.report_progress(done, total, "Measuring scopes")

    answer = _state.host().scopes(request, cancellation_task,
                                  progress if task is not None else None)
    if not answer["succeeded"]:
        raise CancelledError(answer["diagnostic"])
    return _state.freeze(answer)


def read(frame=None, *, last_frame=None, composition=None, time=None, scopes=None,
         signal="display", axis_maximum=1, region=None, ignore_transparent=False,
         resolution="full", display=None, view=None, result="tables", encoding="dense"):
    """Read the scopes of `frame` (or `frame` to `last_frame`, inclusive) of `composition`.

    `result` is "tables" (the count tables of `scopes`, a list of "waveform", "parade",
    "vectorscope", "histogram", "chromaticity"; default all), or "summary" (per channel min, max,
    mean, p1, p50 and p99 code values and clipped counts, and the vectorscope's mean saturation and
    hue). `signal` is "display" or "working" (with `axis_maximum` 1, 2, 4, 8 or 16). `region` is
    (x, y, width, height) in composition pixels. `encoding` "sparse" lists only the non-zero cells
    of a table as [index, count] pairs. A range is at most 1000 frames.
    """
    if result not in ("tables", "summary"):
        raise ValueError("result is 'tables' or 'summary'; bloom.scopes.image writes a picture")
    return _run(_request(frame, last_frame, composition, time, scopes, signal, axis_maximum,
                         region, ignore_transparent, resolution, display, view, result, encoding,
                         "all", None, None, "rgb", "rgb", "linear", 1.0, None, False))


def image(out, frame=None, *, last_frame=None, composition=None, time=None, image="all",
          width=None, height=None, signal="display", axis_maximum=1, region=None,
          ignore_transparent=False, resolution="full", display=None, view=None,
          waveform_channels="rgb", histogram_channels="rgb", histogram_scale="linear",
          brightness=1.0, replace=False):
    """Draw a scope as a PNG at `out` (a range writes one numbered file per frame).

    `image` is "waveform", "parade", "vectorscope", "histogram", "chromaticity" or "all" (the five
    stacked in the panel's order). `width` is 128 to 2048 pixels (default 640); `height` applies to
    one scope (128 to 2048). The picture is drawn as the panel draws it. Returns the same dict as
    `read`, with each frame's `image` (path, bytes, digest, width, height).
    """
    return _run(_request(frame, last_frame, composition, time, None, signal, axis_maximum, region,
                         ignore_transparent, resolution, display, view, "image", "dense", image,
                         width, height, waveform_channels, histogram_channels, histogram_scale,
                         brightness, out, replace))
