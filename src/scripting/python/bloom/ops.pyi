# Generated from the host operation registry by the bloom.scripting.python-api test.
from collections.abc import Mapping
from fractions import Fraction
from typing import Any

from .data import Proxy

Time = int | tuple[int, int] | Fraction | float
Value = float | bool | str | tuple[float, ...]
Path = Mapping[str, Any]

class Operation:
    id: str
    example: str
    schema: Mapping[str, Any]
    def __call__(self, *args: Any, **kwargs: Any) -> Mapping[str, Any] | None: ...

class AnimationOps:
    def convert_to_constant(self, parameter: int | Proxy, value: Value, *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.animation.convert-to-constant  (operation, animation)
        Stops animating a parameter: removes its curve and keeps one constant value, as turning off the
        key diamond in Properties does.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          parameter  id  required
              The animated parameter to freeze.
              Unit: parameter id (query:layers .parameters, query:nodes .parameters)
              Range: >= 1
          value  value  required
              The constant value to keep.
              Unit: the parameter's own width: a number, [x, y], [x, y, z] or [r, g, b, a]

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.command-rejected
              When: The parameter is not animated
              Do: read its source with query {kind:"parameter"}; only a curve-driven parameter converts.
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.animation.convert-to-constant","args":{"parameter":1,"value":0}}]}}
          Python:
            bloom.ops.animation.convert_to_constant(parameter=1, value=0)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.animation.convert-to-constant","args":{"parameter":1,"value":0}}]

        Related: bloom.animation.create-for-parameter, bloom.parameter.set-source, query:keyframes,
        transact
        """
        ...
    def create_for_parameter(self, parameter: int | Proxy, component: int = ..., *, composition: int | Proxy = ..., time: Time = ...) -> Mapping[str, Any] | None:
        """bloom.animation.create-for-parameter  (operation, animation)
        Makes a constant parameter animated: creates its curve with one key holding the current value at
        `time`, as the first click on a key diamond does. Pass `component` to animate one axis or
        channel of a vector or colour parameter.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          parameter  id  required
              The parameter to make animated.
              Unit: parameter id (query:layers .parameters, query:nodes .parameters)
              Range: >= 1
          time  time  optional, defaults to the playhead (frame 0 headless) (context time)
              An exact time. Defaults to the current playhead time.
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]
          component  int  optional, default the whole value (every component)
              Component index: X=0, Y=1, Z=2, Red=3, Green=4, Blue=5, Alpha=6. Omit for a whole-value
              address.
              Range: 0..6
              Values:
                0  X
                1  Y
                2  Z
                3  Red
                4  Green
                5  Blue
                6  Alpha

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names:
          animationCurve, keyframe (each {operationIndex, name, id}).

        Errors:
          bloom.scripting.command-rejected
              When: The parameter is already animated
              Do: key it with bloom.animation.set-keyframe-at-time-for-parameter instead.
          bloom.scripting.command-rejected
              When: The parameter cannot be animated
              Do: query {kind:"parameter"} reports animatable:false for it.
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.animation.create-for-parameter","args":{"parameter":1}}]}}
          Python:
            bloom.ops.animation.create_for_parameter(parameter=1)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.animation.create-for-parameter","args":{"parameter":1}}]

        Related: bloom.animation.set-keyframe-at-time-for-parameter,
        bloom.animation.convert-to-constant, query:keyframes, transact
        """
        ...
    def delete_keyframe(self, curve: int | Proxy, keyframe: int | Proxy, component: int = ..., *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.animation.delete-keyframe  (operation, animation)
        Deletes one key from a curve (one component of a vector or colour curve when `component` is
        given).

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          curve  id  required
              The animation curve that owns the key.
              Unit: animation curve id (query:keyframes, or the parameter's curveId)
              Range: >= 1
          keyframe  id  required
              The key to delete.
              Unit: keyframe id (query:keyframes)
              Range: >= 1
          component  int  optional, default the whole value (every component)
              Component index: X=0, Y=1, Z=2, Red=3, Green=4, Blue=5, Alpha=6. Omit for a whole-value
              address.
              Range: 0..6
              Values:
                0  X
                1  Y
                2  Z
                3  Red
                4  Green
                5  Blue
                6  Alpha

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.animation.delete-keyframe","args":{"curve":1,"keyframe":1}}]}}
          Python:
            bloom.ops.animation.delete_keyframe(curve=1, keyframe=1)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.animation.delete-keyframe","args":{"curve":1,"keyframe":1}}]

        Related: bloom.animation.delete-keyframes, query:keyframes, transact
        """
        ...
    def delete_keyframes(self, keys: tuple, *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.animation.delete-keyframes  (operation, animation)
        Deletes several keys in one step. Each address is [curve, keyframe] or [curve, keyframe,
        component].

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          keys  array  required
              Key addresses as (curve, keyframe) or (curve, keyframe, component).

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.animation.delete-keyframes","args":{"keys":[[1,1]]}}]}}
          Python:
            bloom.ops.animation.delete_keyframes(keys=((1, 1),))
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.animation.delete-keyframes","args":{"keys":[[1,1]]}}]

        Related: bloom.animation.delete-keyframe, query:keyframes, transact
        """
        ...
    def insert_color4_keyframe(self, curve: int | Proxy, value: tuple[float, float, float, float], interpolation: str = ..., *, composition: int | Proxy = ..., time: Time = ...) -> Mapping[str, Any] | None:
        """bloom.animation.insert-color4-keyframe  (operation, animation)
        Adds a key to all four channels of a colour curve at `time`.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          curve  id  required
              The colour curve to key.
              Unit: animation curve id (query:keyframes, or the parameter's curveId)
              Range: >= 1
          time  time  optional, defaults to the playhead (frame 0 headless) (context time)
              An exact time. Defaults to the current playhead time.
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]
          value  color4  required
              The colour at that time.
              Unit: straight (unpremultiplied) RGBA in the composition's working colour space; red,
              green and blue are finite and may exceed 1, alpha is 0..1
          interpolation  str  optional, default linear
              Outgoing ease: "hold", "linear" or "ease-in-out". Defaults to "linear".
              Values:
                "hold"  Hold
                "linear"  Linear
                "ease-in-out"  Ease In-Out

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: keyframe (each
          {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.animation.insert-color4-keyframe","args":{"curve":1,"value":[1,0,0,1]}}]}}
          Python:
            bloom.ops.animation.insert_color4_keyframe(curve=1, value=(1, 0, 0, 1))
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.animation.insert-color4-keyframe","args":{"curve":1,"value":[1,0,0,1]}}]

        Related: bloom.animation.set-keyframe-at-time, query:keyframes, transact
        """
        ...
    def insert_scalar_keyframe(self, curve: int | Proxy, value: float, interpolation: str = ..., *, composition: int | Proxy = ..., time: Time = ...) -> Mapping[str, Any] | None:
        """bloom.animation.insert-scalar-keyframe  (operation, animation)
        Adds a key to a scalar curve at `time`.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          curve  id  required
              The scalar curve to key.
              Unit: animation curve id (query:keyframes, or the parameter's curveId)
              Range: >= 1
          time  time  optional, defaults to the playhead (frame 0 headless) (context time)
              An exact time. Defaults to the current playhead time.
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]
          value  float  required
              The scalar value at that time.
              Unit: the animated parameter's own stored units (query:parameter reports them)
          interpolation  str  optional, default linear
              Outgoing ease: "hold", "linear" or "ease-in-out". Defaults to "linear".
              Values:
                "hold"  Hold
                "linear"  Linear
                "ease-in-out"  Ease In-Out

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: keyframe (each
          {operationIndex, name, id}).

        Errors:
          bloom.scripting.command-rejected
              When: A key already exists at that time
              Do: use bloom.animation.update-scalar-keyframe or bloom.animation.set-keyframe-at-time,
              which updates it.
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.animation.insert-scalar-keyframe","args":{"curve":1,"value":0}}]}}
          Python:
            bloom.ops.animation.insert_scalar_keyframe(curve=1, value=0)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.animation.insert-scalar-keyframe","args":{"curve":1,"value":0}}]

        Related: bloom.animation.set-keyframe-at-time, query:keyframes, transact
        """
        ...
    def insert_vec2_keyframe(self, curve: int | Proxy, value: tuple[float, float], interpolation: str = ..., *, composition: int | Proxy = ..., time: Time = ...) -> Mapping[str, Any] | None:
        """bloom.animation.insert-vec2-keyframe  (operation, animation)
        Adds a key to both components of a vec2 curve (for example Position) at `time`.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          curve  id  required
              The vec2 curve to key.
              Unit: animation curve id (query:keyframes, or the parameter's curveId)
              Range: >= 1
          time  time  optional, defaults to the playhead (frame 0 headless) (context time)
              An exact time. Defaults to the current playhead time.
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]
          value  vec2  required
              The value at that time.
              Unit: the animated parameter's own stored units (query:parameter reports them)
          interpolation  str  optional, default linear
              Outgoing ease: "hold", "linear" or "ease-in-out". Defaults to "linear".
              Values:
                "hold"  Hold
                "linear"  Linear
                "ease-in-out"  Ease In-Out

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: keyframe (each
          {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.animation.insert-vec2-keyframe","args":{"curve":1,"value":[0,0]}}]}}
          Python:
            bloom.ops.animation.insert_vec2_keyframe(curve=1, value=(0, 0))
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.animation.insert-vec2-keyframe","args":{"curve":1,"value":[0,0]}}]

        Related: bloom.animation.set-keyframe-at-time, query:keyframes, transact
        """
        ...
    def insert_vec3_keyframe(self, curve: int | Proxy, value: tuple[float, float, float], interpolation: str = ..., *, composition: int | Proxy = ..., time: Time = ...) -> Mapping[str, Any] | None:
        """bloom.animation.insert-vec3-keyframe  (operation, animation)
        Adds a key to all three components of a vec3 curve at `time`.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          curve  id  required
              The vec3 curve to key.
              Unit: animation curve id (query:keyframes, or the parameter's curveId)
              Range: >= 1
          time  time  optional, defaults to the playhead (frame 0 headless) (context time)
              An exact time. Defaults to the current playhead time.
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]
          value  vec3  required
              The value at that time.
              Unit: the animated parameter's own stored units (query:parameter reports them)
          interpolation  str  optional, default linear
              Outgoing ease: "hold", "linear" or "ease-in-out". Defaults to "linear".
              Values:
                "hold"  Hold
                "linear"  Linear
                "ease-in-out"  Ease In-Out

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: keyframe (each
          {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.animation.insert-vec3-keyframe","args":{"curve":1,"value":[0,0,0]}}]}}
          Python:
            bloom.ops.animation.insert_vec3_keyframe(curve=1, value=(0, 0, 0))
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.animation.insert-vec3-keyframe","args":{"curve":1,"value":[0,0,0]}}]

        Related: bloom.animation.set-keyframe-at-time, query:keyframes, transact
        """
        ...
    def move_keyframes(self, keys: tuple, *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.animation.move-keyframes  (operation, animation)
        Moves keys to new times in one step. Each move is [curve, keyframe, time] or [curve, keyframe,
        time, component]; `time` is the key's new absolute time.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          keys  array  required
              Moves as (curve, keyframe, time) or (curve, keyframe, time, component).

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.command-rejected
              When: Two keys would land on one time
              Do: move them in separate transactions or choose distinct times.
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.animation.move-keyframes","args":{"keys":[[1,1,12]]}}]}}
          Python:
            bloom.ops.animation.move_keyframes(keys=((1, 1, 12),))
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.animation.move-keyframes","args":{"keys":[[1,1,12]]}}]

        Related: query:keyframes, transact
        """
        ...
    def paste_keyframes(self, keys: tuple, *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.animation.paste-keyframes  (operation, animation)
        Writes keys onto parameters, creating their curves when needed, as pasting keys at the playhead
        does. Each entry is [parameter, time, value] plus an optional component and interpolation.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          keys  array  required
              Pastes as (parameter, time, value), plus optional component and interpolation.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: keyframe (one
          per pasted key) (each {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.animation.paste-keyframes","args":{"keys":[[1,12,1]]}}]}}
          Python:
            bloom.ops.animation.paste_keyframes(keys=((1, 12, 1),))
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.animation.paste-keyframes","args":{"keys":[[1,12,1]]}}]

        Related: bloom.animation.set-keyframe-at-time-for-parameter, transact
        """
        ...
    def set_keyframe_at_time(self, curve: int | Proxy, value: Value, *, composition: int | Proxy = ..., time: Time = ...) -> Mapping[str, Any] | None:
        """bloom.animation.set-keyframe-at-time  (operation, animation)
        Sets a curve's value at `time`: updates the key there, or inserts one. The value has the curve's
        own width (a number, [x, y], [x, y, z] or [r, g, b, a]).

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          curve  id  required
              The curve to key.
              Unit: animation curve id (query:keyframes, or the parameter's curveId)
              Range: >= 1
          time  time  optional, defaults to the playhead (frame 0 headless) (context time)
              An exact time. Defaults to the current playhead time.
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]
          value  value  required
              The value at that time.
              Unit: the parameter's own width: a number, [x, y], [x, y, z] or [r, g, b, a]

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: keyframe (each
          {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.animation.set-keyframe-at-time","args":{"curve":1,"value":0}}]}}
          Python:
            bloom.ops.animation.set_keyframe_at_time(curve=1, value=0)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.animation.set-keyframe-at-time","args":{"curve":1,"value":0}}]

        Related: bloom.animation.set-keyframe-at-time-for-parameter, query:keyframes, transact
        """
        ...
    def set_keyframe_at_time_for_parameter(self, parameter: int | Proxy, value: Value, *, composition: int | Proxy = ..., time: Time = ...) -> Mapping[str, Any] | None:
        """bloom.animation.set-keyframe-at-time-for-parameter  (operation, animation)
        Sets an animated parameter's value at `time`: updates the key there or inserts one. The
        parameter must already be animated; make a constant one animated first with
        bloom.animation.create-for-parameter (in the same transaction is fine). The value has the
        parameter's own width (query {kind:"parameter"} reports valueKind).

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          parameter  id  required
              The parameter to key. A constant parameter becomes animated in the same undo step, its
              constant as the first key.
              Unit: parameter id (query:layers .parameters, query:nodes .parameters)
              Range: >= 1
          time  time  optional, defaults to the playhead (frame 0 headless) (context time)
              An exact time. Defaults to the current playhead time.
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]
          value  value  required
              The value at that time.
              Unit: the parameter's own width: a number, [x, y], [x, y, z] or [r, g, b, a]

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: keyframe (each
          {operationIndex, name, id}).

        Errors:
          bloom.scripting.command-rejected
              When: The parameter is not animated ("Parameter does not have an animation source")
              Do: run bloom.animation.create-for-parameter first.
          bloom.scripting.command-rejected
              When: The value has the wrong width
              Do: match the parameter's valueKind: scalar is a number, vector2 is [x, y], vector3 [x, y,
              z], color [r, g, b, a].
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.animation.set-keyframe-at-time-for-parameter","args":{"parameter":1,"value":0}}]}}
          Python:
            bloom.ops.animation.set_keyframe_at_time_for_parameter(parameter=1, value=0)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.animation.set-keyframe-at-time-for-parameter","args":{"parameter":1,"value":0}}]

        Related: bloom.animation.create-for-parameter,
        bloom.animation.set-keyframe-at-time-for-parameter-component, query:keyframes,
        bloom.parameter.set-source, transact
        """
        ...
    def set_keyframe_at_time_for_parameter_component(self, parameter: int | Proxy, component: int, value: float, *, composition: int | Proxy = ..., time: Time = ...) -> Mapping[str, Any] | None:
        """bloom.animation.set-keyframe-at-time-for-parameter-component  (operation, animation)
        Keys one axis or channel of an animated vector or colour parameter at `time`, leaving the other
        components as they are. Make a constant parameter animated first with
        bloom.animation.create-for-parameter.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          parameter  id  required
              The parameter to key.
              Unit: parameter id (query:layers .parameters, query:nodes .parameters)
              Range: >= 1
          component  int  required
              Component index: X=0, Y=1, Z=2, Red=3, Green=4, Blue=5, Alpha=6.
              Range: 0..6
              Values:
                0  X
                1  Y
                2  Z
                3  Red
                4  Green
                5  Blue
                6  Alpha
          time  time  optional, defaults to the playhead (frame 0 headless) (context time)
              An exact time. Defaults to the current playhead time.
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]
          value  float  required
              The scalar value at that time.
              Unit: the animated parameter's own stored units (query:parameter reports them)

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: keyframe (each
          {operationIndex, name, id}).

        Errors:
          bloom.scripting.command-rejected
              When: The parameter is not animated
              Do: run bloom.animation.create-for-parameter first.
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.animation.set-keyframe-at-time-for-parameter-component","args":{"parameter":1,"component":0,"value":400}}]}}
          Python:
            bloom.ops.animation.set_keyframe_at_time_for_parameter_component(parameter=1, component=0, value=400)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.animation.set-keyframe-at-time-for-parameter-component","args":{"parameter":1,"component":0,"value":400}}]

        Related: bloom.animation.create-for-parameter,
        bloom.animation.set-keyframe-at-time-for-parameter, query:keyframes, transact
        """
        ...
    def set_keyframe_handles(self, outgoing: tuple = ..., incoming: tuple = ..., *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.animation.set-keyframe-handles  (operation, animation)
        Sets ease handles. Each handle is [curve, keyframe, time, value] with an optional component:
        `time` is a fraction 0..1 of the segment's duration measured from the key, `value` an offset
        from the key's value. Handles shape ease-in-out segments only.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          outgoing  array  optional
              Outgoing handles as (curve, keyframe, time, value) with an optional component.
          incoming  array  optional
              Incoming handles as (curve, keyframe, time, value) with an optional component.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.animation.set-keyframe-handles","args":{"outgoing":[[1,1,0.25,0]]}}]}}
          Python:
            bloom.ops.animation.set_keyframe_handles(outgoing=((1, 1, 0.25, 0),))
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.animation.set-keyframe-handles","args":{"outgoing":[[1,1,0.25,0]]}}]

        Related: bloom.animation.set-keyframe-interpolation, query:keyframes, transact
        """
        ...
    def set_keyframe_interpolation(self, curve: int | Proxy, keyframe: int | Proxy, interpolation: str, component: int = ..., *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.animation.set-keyframe-interpolation  (operation, animation)
        Sets the outgoing interpolation of one key: the segment from this key to the next.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          curve  id  required
              The curve to edit.
              Unit: animation curve id (query:keyframes, or the parameter's curveId)
              Range: >= 1
          keyframe  id  required
              The key whose outgoing ease changes.
              Unit: keyframe id (query:keyframes)
              Range: >= 1
          interpolation  str  required
              "hold", "linear" or "ease-in-out".
              Values:
                "hold"  Hold
                "linear"  Linear
                "ease-in-out"  Ease In-Out
          component  int  optional, default the whole value (every component)
              Component index: X=0, Y=1, Z=2, Red=3, Green=4, Blue=5, Alpha=6. Omit for a whole-value
              address.
              Range: 0..6
              Values:
                0  X
                1  Y
                2  Z
                3  Red
                4  Green
                5  Blue
                6  Alpha

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.animation.set-keyframe-interpolation","args":{"curve":1,"keyframe":1,"interpolation":"ease-in-out"}}]}}
          Python:
            bloom.ops.animation.set_keyframe_interpolation(curve=1, keyframe=1, interpolation="ease-in-out")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.animation.set-keyframe-interpolation","args":{"curve":1,"keyframe":1,"interpolation":"ease-in-out"}}]

        Related: bloom.animation.set-keyframes-interpolation, query:keyframes, transact
        """
        ...
    def set_keyframe_values(self, values: tuple, *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.animation.set-keyframe-values  (operation, animation)
        Changes the values of several keys in one step without moving them. Each edit is [curve,
        keyframe, value] or [curve, keyframe, value, component].

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          values  array  required
              Edits as (curve, keyframe, value) or (curve, keyframe, value, component).

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.animation.set-keyframe-values","args":{"values":[[1,1,1]]}}]}}
          Python:
            bloom.ops.animation.set_keyframe_values(values=((1, 1, 1),))
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.animation.set-keyframe-values","args":{"values":[[1,1,1]]}}]

        Related: query:keyframes, transact
        """
        ...
    def set_keyframes_interpolation(self, keys: tuple, interpolation: str, *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.animation.set-keyframes-interpolation  (operation, animation)
        Sets the outgoing interpolation of several keys in one step.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          keys  array  required
              Key addresses as (curve, keyframe) or (curve, keyframe, component).
          interpolation  str  required
              "hold", "linear" or "ease-in-out".
              Values:
                "hold"  Hold
                "linear"  Linear
                "ease-in-out"  Ease In-Out

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.animation.set-keyframes-interpolation","args":{"keys":[[1,1]],"interpolation":"ease-in-out"}}]}}
          Python:
            bloom.ops.animation.set_keyframes_interpolation(keys=((1, 1),), interpolation="ease-in-out")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.animation.set-keyframes-interpolation","args":{"keys":[[1,1]],"interpolation":"ease-in-out"}}]

        Related: bloom.animation.set-keyframe-interpolation, query:keyframes, transact
        """
        ...
    def update_color4_keyframe(self, curve: int | Proxy, keyframe: int | Proxy, value: tuple[float, float, float, float], interpolation: str = ..., *, composition: int | Proxy = ..., time: Time = ...) -> Mapping[str, Any] | None:
        """bloom.animation.update-color4-keyframe  (operation, animation)
        Re-places one colour key: its time, value and outgoing interpolation.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          curve  id  required
              The curve to edit.
              Unit: animation curve id (query:keyframes, or the parameter's curveId)
              Range: >= 1
          keyframe  id  required
              The key to re-place.
              Unit: keyframe id (query:keyframes)
              Range: >= 1
          time  time  optional, defaults to the playhead (frame 0 headless) (context time)
              An exact time. Defaults to the current playhead time.
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]
          value  color4  required
              The new colour.
              Unit: straight (unpremultiplied) RGBA in the composition's working colour space; red,
              green and blue are finite and may exceed 1, alpha is 0..1
          interpolation  str  optional, default linear
              Outgoing ease: "hold", "linear" or "ease-in-out". Defaults to "linear".
              Values:
                "hold"  Hold
                "linear"  Linear
                "ease-in-out"  Ease In-Out

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: keyframe (each
          {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.animation.update-color4-keyframe","args":{"curve":1,"keyframe":1,"value":[1,0,0,1]}}]}}
          Python:
            bloom.ops.animation.update_color4_keyframe(curve=1, keyframe=1, value=(1, 0, 0, 1))
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.animation.update-color4-keyframe","args":{"curve":1,"keyframe":1,"value":[1,0,0,1]}}]

        Related: query:keyframes, transact
        """
        ...
    def update_scalar_keyframe(self, curve: int | Proxy, keyframe: int | Proxy, value: float, interpolation: str = ..., *, composition: int | Proxy = ..., time: Time = ...) -> Mapping[str, Any] | None:
        """bloom.animation.update-scalar-keyframe  (operation, animation)
        Re-places one scalar key: its time, value and outgoing interpolation.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          curve  id  required
              The curve to edit.
              Unit: animation curve id (query:keyframes, or the parameter's curveId)
              Range: >= 1
          keyframe  id  required
              The key to re-place.
              Unit: keyframe id (query:keyframes)
              Range: >= 1
          time  time  optional, defaults to the playhead (frame 0 headless) (context time)
              An exact time. Defaults to the current playhead time.
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]
          value  float  required
              The new value.
              Unit: the animated parameter's own stored units (query:parameter reports them)
          interpolation  str  optional, default linear
              Outgoing ease: "hold", "linear" or "ease-in-out". Defaults to "linear".
              Values:
                "hold"  Hold
                "linear"  Linear
                "ease-in-out"  Ease In-Out

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: keyframe (each
          {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.animation.update-scalar-keyframe","args":{"curve":1,"keyframe":1,"value":0}}]}}
          Python:
            bloom.ops.animation.update_scalar_keyframe(curve=1, keyframe=1, value=0)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.animation.update-scalar-keyframe","args":{"curve":1,"keyframe":1,"value":0}}]

        Related: query:keyframes, transact
        """
        ...
    def update_vec2_keyframe(self, curve: int | Proxy, keyframe: int | Proxy, value: tuple[float, float], interpolation: str = ..., *, composition: int | Proxy = ..., time: Time = ...) -> Mapping[str, Any] | None:
        """bloom.animation.update-vec2-keyframe  (operation, animation)
        Re-places one vec2 key: its time, value and outgoing interpolation.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          curve  id  required
              The curve to edit.
              Unit: animation curve id (query:keyframes, or the parameter's curveId)
              Range: >= 1
          keyframe  id  required
              The key to re-place.
              Unit: keyframe id (query:keyframes)
              Range: >= 1
          time  time  optional, defaults to the playhead (frame 0 headless) (context time)
              An exact time. Defaults to the current playhead time.
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]
          value  vec2  required
              The new value.
              Unit: the animated parameter's own stored units (query:parameter reports them)
          interpolation  str  optional, default linear
              Outgoing ease: "hold", "linear" or "ease-in-out". Defaults to "linear".
              Values:
                "hold"  Hold
                "linear"  Linear
                "ease-in-out"  Ease In-Out

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: keyframe (each
          {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.animation.update-vec2-keyframe","args":{"curve":1,"keyframe":1,"value":[0,0]}}]}}
          Python:
            bloom.ops.animation.update_vec2_keyframe(curve=1, keyframe=1, value=(0, 0))
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.animation.update-vec2-keyframe","args":{"curve":1,"keyframe":1,"value":[0,0]}}]

        Related: query:keyframes, transact
        """
        ...
    def update_vec3_keyframe(self, curve: int | Proxy, keyframe: int | Proxy, value: tuple[float, float, float], interpolation: str = ..., *, composition: int | Proxy = ..., time: Time = ...) -> Mapping[str, Any] | None:
        """bloom.animation.update-vec3-keyframe  (operation, animation)
        Re-places one vec3 key: its time, value and outgoing interpolation.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          curve  id  required
              The curve to edit.
              Unit: animation curve id (query:keyframes, or the parameter's curveId)
              Range: >= 1
          keyframe  id  required
              The key to re-place.
              Unit: keyframe id (query:keyframes)
              Range: >= 1
          time  time  optional, defaults to the playhead (frame 0 headless) (context time)
              An exact time. Defaults to the current playhead time.
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]
          value  vec3  required
              The new value.
              Unit: the animated parameter's own stored units (query:parameter reports them)
          interpolation  str  optional, default linear
              Outgoing ease: "hold", "linear" or "ease-in-out". Defaults to "linear".
              Values:
                "hold"  Hold
                "linear"  Linear
                "ease-in-out"  Ease In-Out

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: keyframe (each
          {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.animation.update-vec3-keyframe","args":{"curve":1,"keyframe":1,"value":[0,0,0]}}]}}
          Python:
            bloom.ops.animation.update_vec3_keyframe(curve=1, keyframe=1, value=(0, 0, 0))
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.animation.update-vec3-keyframe","args":{"curve":1,"keyframe":1,"value":[0,0,0]}}]

        Related: query:keyframes, transact
        """
        ...

animation: AnimationOps

class AssetOps:
    def create_folder(self, name: str, parent: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.asset.create-folder  (operation, asset)
        Creates an Assets folder.

        Arguments:
          name  str  required
              The folder name.
          parent  id  optional
              The containing folder. Omit for the project root.
              Unit: layer id (query:layers .id)
              Range: >= 1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: folder (each
          {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.asset.create-folder","args":{"name":"Footage"}}]}}
          Python:
            bloom.ops.asset.create_folder(name="Footage")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.asset.create-folder","args":{"name":"Footage"}}]

        Related: bloom.asset.move, bloom.asset.move-folders, query:assets, transact
        """
        ...
    def delete_items(self, assets: tuple = ..., compositions: tuple = ..., folders: tuple = ..., withContents: bool = ...) -> Mapping[str, Any] | None:
        """bloom.asset.delete-items  (operation, asset)
        Deletes Assets items as Delete in the Assets panel does, in one undo step: the assets, then the
        compositions, then the folders. withContents=false (the default) removes each folder alone and
        moves what it held to its parent folder; withContents=true also deletes every asset, composition
        and subfolder inside it. Nothing asks first: the panel's "used item" warning is the caller's to
        give (layers and nodes that used a deleted item keep referring to it and show it as missing).
        Deleting the composition the editor shows switches the editor to another one. Outputs: none.
        Errors: "Nothing to delete" - all three arrays empty; "The folder does not exist"; an unknown
        asset or composition id is rejected by its remove command; deleting the last composition is
        refused. Undo: one step restores every item with its id and folder. MCP example:
        {"op":"bloom.asset.delete-items","args":{"folders":[2],"withContents":true}}.

        Arguments:
          assets  array  optional
              Array of asset ids. Default: none.
          compositions  array  optional
              Array of composition ids. Default: none.
          folders  array  optional
              Array of Assets folder ids. Default: none.
          withContents  bool  optional
              Boolean. true also deletes everything inside the folders. Default false.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.asset.delete-items","args":{"assets":[1]}}]}}
          Python:
            bloom.ops.asset.delete_items(assets=(1,))
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.asset.delete-items","args":{"assets":[1]}}]

        Related: transact
        """
        ...
    def ensure_font(self, path: str, digest: str, family: str = ..., style: str = ..., index: int = ..., portability: str = ..., relinkHint: str = ..., name: str = ..., tags: tuple = ...) -> Mapping[str, Any] | None:
        """bloom.asset.ensure-font  (operation, asset)
        Adds a font face as an asset, or returns the existing asset with the same digest, so a Text
        layer can use it.

        Arguments:
          path  str  required
              The face's file path, or an embedded locator path.
          digest  str  required
              The face's SHA-256 content digest, 64 lowercase hex characters.
              Unit: 64 lowercase hexadecimal characters (SHA-256)
          family  str  optional
              The family name.
          style  str  optional
              The style name.
          index  int  optional
              The face index inside a collection file.
              Unit: zero-based position
              Range: >= 0
          portability  str  optional, default system
              Locator portability: "system", "builtin" or "project-relative".
              Values:
                "system"  System font
                "builtin"  Built-in font
                "project-relative"  Project-relative file
          relinkHint  str  optional
              A relink hint to remember.
          name  str  optional
              The asset name.
          tags  array  optional
              Asset tags.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: asset (each
          {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.asset.ensure-font","args":{"path":"/usr/share/fonts/DejaVuSans.ttf","digest":"0000000000000000000000000000000000000000000000000000000000000000"}}]}}
          Python:
            bloom.ops.asset.ensure_font(path="/usr/share/fonts/DejaVuSans.ttf", digest="0000000000000000000000000000000000000000000000000000000000000000")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.asset.ensure-font","args":{"path":"/usr/share/fonts/DejaVuSans.ttf","digest":"0000000000000000000000000000000000000000000000000000000000000000"}}]

        Related: bloom.asset.relink-font, query:assets, transact
        """
        ...
    def import_(self, paths: tuple, projectDirectory: str, folder: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.asset.import  (operation, asset)
        Imports media files and folders into Assets in one step: hashes and probes each file (image
        sequences are detected from numbered names) and adds one asset per item. Files the project
        already holds are reused, not duplicated. It does not add layers; follow with
        bloom.layer.add-image or bloom.layer.add-audio.

        Arguments:
          paths  array  required
              Absolute or project-relative media files or folders to import.
          projectDirectory  str  required
              The project directory the locators are made relative to.
              Unit: directory path; relative locators are made relative to it
          folder  id  optional
              The Assets folder the new assets land in. Omit for the project root.
              Unit: Assets folder id (query:assets .folder)
              Range: >= 1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: asset (one per
          imported or reused item) (each {operationIndex, name, id}).

        Errors:
          bloom.scripting.operation-warning
              When: A path cannot be read or is not a supported format
              Do: it is reported as a bloom.scripting.operation-warning diagnostic and skipped; fix the
              path and import it again.
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.asset.import","args":{"paths":["footage/plate.exr"],"projectDirectory":"."}}]}}
          Python:
            bloom.ops.asset.import_(paths=("footage/plate.exr",), projectDirectory=".")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.asset.import","args":{"paths":["footage/plate.exr"],"projectDirectory":"."}}]

        Related: bloom.layer.add-image, bloom.layer.add-audio, query:assets, transact
        """
        ...
    def move(self, assets: tuple, folder: int | Proxy = ..., index: int = ...) -> Mapping[str, Any] | None:
        """bloom.asset.move  (operation, asset)
        Moves assets into a folder at a position.

        Arguments:
          assets  array  required
              The assets to move.
          folder  id  optional
              The destination folder. Omit for the project root.
              Unit: Assets folder id (query:assets .folder)
              Range: >= 1
          index  int  optional
              Destination position after the moved assets are removed. Defaults to 0.
              Unit: zero-based position
              Range: >= 0

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.asset.move","args":{"assets":[1]}}]}}
          Python:
            bloom.ops.asset.move(assets=(1,))
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.asset.move","args":{"assets":[1]}}]

        Related: bloom.asset.reorder, query:assets, transact
        """
        ...
    def move_compositions(self, compositions: tuple, folder: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.asset.move-compositions  (operation, asset)
        Files compositions into an Assets folder.

        Arguments:
          compositions  array  required
              The compositions to move.
          folder  id  optional
              The destination folder. Omit for the project root.
              Unit: Assets folder id (query:assets .folder)
              Range: >= 1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.asset.move-compositions","args":{"compositions":[1]}}]}}
          Python:
            bloom.ops.asset.move_compositions(compositions=(1,))
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.asset.move-compositions","args":{"compositions":[1]}}]

        Related: query:compositions, transact
        """
        ...
    def move_folders(self, folders: tuple, parent: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.asset.move-folders  (operation, asset)
        Moves folders under another folder.

        Arguments:
          folders  array  required
              The folders to move.
          parent  id  optional
              The destination folder. Omit for the project root.
              Unit: layer id (query:layers .id)
              Range: >= 1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.asset.move-folders","args":{"folders":[1]}}]}}
          Python:
            bloom.ops.asset.move_folders(folders=(1,))
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.asset.move-folders","args":{"folders":[1]}}]

        Related: bloom.asset.create-folder, transact
        """
        ...
    def relink(self, asset: int | Proxy, path: str, projectDirectory: str) -> Mapping[str, Any] | None:
        """bloom.asset.relink  (operation, asset)
        Points an asset at a different media file (Replace Footage), keeping every layer that uses it.

        Arguments:
          asset  id  required
              The asset to repoint.
              Unit: asset id (query:assets)
              Range: >= 1
          path  str  required
              The replacement media path.
          projectDirectory  str  required
              The project directory the locator is made relative to.
              Unit: directory path; relative locators are made relative to it

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.asset.relink","args":{"asset":1,"path":"footage/plate.exr","projectDirectory":"."}}]}}
          Python:
            bloom.ops.asset.relink(asset=1, path="footage/plate.exr", projectDirectory=".")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.asset.relink","args":{"asset":1,"path":"footage/plate.exr","projectDirectory":"."}}]

        Related: query:assets, transact
        """
        ...
    def relink_font(self, asset: int | Proxy, path: str, digest: str, family: str = ..., style: str = ..., index: int = ..., portability: str = ..., relinkHint: str = ..., name: str = ..., tags: tuple = ...) -> Mapping[str, Any] | None:
        """bloom.asset.relink-font  (operation, asset)
        Points a font asset at another file with the given digest.

        Arguments:
          asset  id  required
              The font asset to repoint.
              Unit: asset id (query:assets)
              Range: >= 1
          path  str  required
              The face's file path, or an embedded locator path.
          digest  str  required
              The face's SHA-256 content digest, 64 lowercase hex characters.
              Unit: 64 lowercase hexadecimal characters (SHA-256)
          family  str  optional
              The family name.
          style  str  optional
              The style name.
          index  int  optional
              The face index inside a collection file.
              Unit: zero-based position
              Range: >= 0
          portability  str  optional, default system
              Locator portability: "system", "builtin" or "project-relative".
              Values:
                "system"  System font
                "builtin"  Built-in font
                "project-relative"  Project-relative file
          relinkHint  str  optional
              A relink hint to remember.
          name  str  optional
              The asset name.
          tags  array  optional
              Asset tags.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: asset (each
          {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.asset.relink-font","args":{"asset":1,"path":"/usr/share/fonts/DejaVuSans.ttf","digest":"0000000000000000000000000000000000000000000000000000000000000000"}}]}}
          Python:
            bloom.ops.asset.relink_font(asset=1, path="/usr/share/fonts/DejaVuSans.ttf", digest="0000000000000000000000000000000000000000000000000000000000000000")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.asset.relink-font","args":{"asset":1,"path":"/usr/share/fonts/DejaVuSans.ttf","digest":"0000000000000000000000000000000000000000000000000000000000000000"}}]

        Related: bloom.asset.ensure-font, transact
        """
        ...
    def remove(self, asset: int | Proxy) -> Mapping[str, Any] | None:
        """bloom.asset.remove  (operation, asset)
        Removes an asset from the project. Layers that use it lose their media.

        Arguments:
          asset  id  required
              The asset to remove.
              Unit: asset id (query:assets)
              Range: >= 1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.asset.remove","args":{"asset":1}}]}}
          Python:
            bloom.ops.asset.remove(asset=1)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.asset.remove","args":{"asset":1}}]

        Related: query:assets, transact
        """
        ...
    def remove_folder(self, folder: int | Proxy) -> Mapping[str, Any] | None:
        """bloom.asset.remove-folder  (operation, asset)
        Removes an Assets folder.

        Arguments:
          folder  id  required
              The folder to remove.
              Unit: Assets folder id (query:assets .folder)
              Range: >= 1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.asset.remove-folder","args":{"folder":1}}]}}
          Python:
            bloom.ops.asset.remove_folder(folder=1)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.asset.remove-folder","args":{"folder":1}}]

        Related: bloom.asset.create-folder, transact
        """
        ...
    def rename(self, asset: int | Proxy, name: str) -> Mapping[str, Any] | None:
        """bloom.asset.rename  (operation, asset)
        Renames an asset.

        Arguments:
          asset  id  required
              The asset to rename.
              Unit: asset id (query:assets)
              Range: >= 1
          name  str  required
              The new name.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.asset.rename","args":{"asset":1,"name":"Plate"}}]}}
          Python:
            bloom.ops.asset.rename(asset=1, name="Plate")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.asset.rename","args":{"asset":1,"name":"Plate"}}]

        Related: query:assets, transact
        """
        ...
    def rename_folder(self, folder: int | Proxy, name: str) -> Mapping[str, Any] | None:
        """bloom.asset.rename-folder  (operation, asset)
        Renames an Assets folder.

        Arguments:
          folder  id  required
              The folder to rename.
              Unit: Assets folder id (query:assets .folder)
              Range: >= 1
          name  str  required
              The new name.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.asset.rename-folder","args":{"folder":1,"name":"Footage"}}]}}
          Python:
            bloom.ops.asset.rename_folder(folder=1, name="Footage")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.asset.rename-folder","args":{"folder":1,"name":"Footage"}}]

        Related: bloom.asset.create-folder, transact
        """
        ...
    def reorder(self, order: tuple, folder: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.asset.reorder  (operation, asset)
        Sets the complete order of the assets in one folder.

        Arguments:
          order  array  required
              The complete asset order for the folder.
          folder  id  optional
              The folder being ordered. Omit for the project root.
              Unit: Assets folder id (query:assets .folder)
              Range: >= 1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.asset.reorder","args":{"order":[1]}}]}}
          Python:
            bloom.ops.asset.reorder(order=(1,))
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.asset.reorder","args":{"order":[1]}}]

        Related: bloom.asset.move, transact
        """
        ...
    def set_input_color(self, asset: int | Proxy, legacy: str = ..., inputColorSpaceId: str = ...) -> Mapping[str, Any] | None:
        """bloom.asset.set-input-color  (operation, asset)
        Sets how an image or video asset's pixels are interpreted (Input Colour): a legacy choice, or an
        exact colour space of the project's OCIO config.

        Arguments:
          asset  id  required
              The image or video asset to interpret.
              Unit: asset id (query:assets)
              Range: >= 1
          legacy  str  optional, default auto
              "auto", "srgb", "linear" or "raw" (lower case). Defaults to "auto".
              Values:
                "auto"  Auto
                "srgb"  sRGB
                "linear"  Linear
                "raw"  Raw
          inputColorSpaceId  str  optional
              An exact colour-space id in the selected OCIO config; clears the legacy choice.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: Both a legacy choice other than auto and inputColorSpaceId are given
              Do: pass one of them.
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.asset.set-input-color","args":{"asset":1}}]}}
          Python:
            bloom.ops.asset.set_input_color(asset=1)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.asset.set-input-color","args":{"asset":1}}]

        Related: query:assets, transact
        """
        ...
    def set_tags(self, assets: tuple, tags: tuple) -> Mapping[str, Any] | None:
        """bloom.asset.set-tags  (operation, asset)
        Replaces the tag list of each asset.

        Arguments:
          assets  array  required
              The assets to tag.
          tags  array  required
              The complete tag list.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.asset.set-tags","args":{"assets":[1],"tags":["plate"]}}]}}
          Python:
            bloom.ops.asset.set_tags(assets=(1,), tags=("plate",))
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.asset.set-tags","args":{"assets":[1],"tags":["plate"]}}]

        Related: query:assets, transact
        """
        ...

asset: AssetOps

class CompositionOps:
    def add(self, name: str, width: int = ..., height: int = ..., duration: Time = ..., frameRateNumerator: int = ..., frameRateDenominator: int = ..., background: tuple[float, float, float, float] = ...) -> Mapping[str, Any] | None:
        """bloom.composition.add  (operation, composition)
        Creates a composition with its own Merge and Output nodes and no layers.

        Arguments:
          name  str  required
              The new composition's name.
          width  int  optional
              Pixel width. Defaults to 1920.
              Unit: pixels
              Range: 1..1048576
          height  int  optional
              Pixel height. Defaults to 1080.
              Unit: pixels
              Range: 1..1048576
          duration  time  optional
              The duration. Defaults to 48 frames.
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]
          frameRateNumerator  int  optional
              Frame-rate numerator. Defaults to 24.
              Unit: frames per second as numerator / denominator (24000/1001 is 23.976)
              Range: 1..4294967295
          frameRateDenominator  int  optional
              Frame-rate denominator. Defaults to 1.
              Unit: frames per second as numerator / denominator (24000/1001 is 23.976)
              Range: 1..4294967295
          background  color4  optional
              Background colour. Defaults to opaque black.
              Unit: straight (unpremultiplied) RGBA in the composition's working colour space; red,
              green and blue are finite and may exceed 1, alpha is 0..1; display-only: renders, exports
              and captures keep alpha and do not include the background

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: composition
          (each {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.composition.add","args":{"name":"Scene"}}]}}
          Python:
            bloom.ops.composition.add(name="Scene")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.composition.add","args":{"name":"Scene"}}]

        Related: bloom.composition.set-settings, query:compositions, bloom.layer.add-solid, transact
        """
        ...
    def add_from_assets(self, assets: tuple, folder: int | Proxy = ..., *, template: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.composition.add-from-assets  (operation, composition)
        Creates a composition from footage, as New Composition from Selection in the Assets panel does:
        sized, timed and named from the first image, sequence or video asset (the first asset when all
        are audio; a video's frame rate and a video or audio duration are used), with what the footage
        does not state taken from `template`, filed in `folder`, and every footage asset added as a
        layer, the first uppermost. Fonts and LUTs are skipped. It does not make the new composition the
        one the editor shows. Outputs: "composition" (the new id) and one "layer" per footage asset.
        Errors: "Asset N does not exist"; "Name at least one image, sequence, video or audio asset";
        "The template composition does not exist"; "The destination folder does not exist". Undo: one
        step. MCP example: {"op":"bloom.composition.add-from-assets","args":{"assets":[3,4]}}.

        Arguments:
          template  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              Id. The composition whose settings fill what the footage does not state (pixel aspect,
              background, working colour space, safe areas). Defaults to the current composition.
              Unit: stable id (a positive integer)
              Range: >= 1
          assets  array  required
              Array of asset ids; the first visual one sizes the composition.
          folder  id  optional
              Id. The Assets folder the composition is filed in. Default: the Assets root.
              Unit: Assets folder id (query:assets .folder)
              Range: >= 1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.composition.add-from-assets","args":{"assets":[1]}}]}}
          Python:
            bloom.ops.composition.add_from_assets(assets=(1,))
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.composition.add-from-assets","args":{"assets":[1]}}]

        Related: transact
        """
        ...
    def clear_work_area(self, *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.composition.clear-work-area  (operation, composition)
        Clears the work area so playback, RAM preview and work-area exports cover the whole composition.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.composition.clear-work-area","args":{}}]}}
          Python:
            bloom.ops.composition.clear_work_area()
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.composition.clear-work-area","args":{}}]

        Related: bloom.composition.set-work-area, query:compositions, transact
        """
        ...
    def delete(self, *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.composition.delete  (operation, composition)
        Deletes a composition. Deleting the last composition leaves an empty project.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.composition.delete","args":{}}]}}
          Python:
            bloom.ops.composition.delete()
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.composition.delete","args":{}}]

        Related: query:compositions, transact
        """
        ...
    def duplicate(self, *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.composition.duplicate  (operation, composition)
        Copies a composition with its layers, nodes, parameters and keys.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: composition
          (each {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.composition.duplicate","args":{}}]}}
          Python:
            bloom.ops.composition.duplicate()
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.composition.duplicate","args":{}}]

        Related: query:compositions, transact
        """
        ...
    def set_background_color(self, color: tuple[float, float, float, float], *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.composition.set-background-color  (operation, composition)
        Sets the composition's background colour. The background is display-only: renders, exports and
        captures keep alpha and do not include it.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          color  color4  required
              The background colour.
              Unit: straight (unpremultiplied) RGBA in the composition's working colour space; red,
              green and blue are finite and may exceed 1, alpha is 0..1; display-only: renders, exports
              and captures keep alpha and do not include the background

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.composition.set-background-color","args":{"color":[0,0,0,1]}}]}}
          Python:
            bloom.ops.composition.set_background_color(color=(0, 0, 0, 1))
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.composition.set-background-color","args":{"color":[0,0,0,1]}}]

        Related: bloom.composition.set-settings, transact
        """
        ...
    def set_duration(self, duration: Time, *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.composition.set-duration  (operation, composition)
        Sets the composition's duration.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          duration  time  required
              The new duration.
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.composition.set-duration","args":{"duration":48}}]}}
          Python:
            bloom.ops.composition.set_duration(duration=48)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.composition.set-duration","args":{"duration":48}}]

        Related: bloom.composition.set-settings, query:compositions, transact
        """
        ...
    def set_format(self, width: int, height: int, frameRateNumerator: int = ..., frameRateDenominator: int = ..., *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.composition.set-format  (operation, composition)
        Sets the composition's pixel size and frame rate.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          width  int  required
              Pixel width.
              Unit: pixels
              Range: 1..1048576
          height  int  required
              Pixel height.
              Unit: pixels
              Range: 1..1048576
          frameRateNumerator  int  optional
              Frame-rate numerator. Defaults to 24.
              Unit: frames per second as numerator / denominator (24000/1001 is 23.976)
              Range: 1..4294967295
          frameRateDenominator  int  optional
              Frame-rate denominator. Defaults to 1.
              Unit: frames per second as numerator / denominator (24000/1001 is 23.976)
              Range: 1..4294967295

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.composition.set-format","args":{"width":1920,"height":1080}}]}}
          Python:
            bloom.ops.composition.set_format(width=1920, height=1080)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.composition.set-format","args":{"width":1920,"height":1080}}]

        Related: bloom.composition.set-settings, query:compositions, transact
        """
        ...
    def set_name(self, name: str, *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.composition.set-name  (operation, composition)
        Renames a composition.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          name  str  required
              The new name.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.composition.set-name","args":{"name":"Scene"}}]}}
          Python:
            bloom.ops.composition.set_name(name="Scene")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.composition.set-name","args":{"name":"Scene"}}]

        Related: query:compositions, transact
        """
        ...
    def set_safe_areas(self, action: float = ..., title: float = ..., *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.composition.set-safe-areas  (operation, composition)
        Sets the action-safe and title-safe guide fractions the Viewer draws. Guides are display-only
        and never rendered.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          action  float  optional
              Action-safe fraction. Defaults to 0.9.
              Unit: fraction of the frame, 0..1
              Range: 0..1
          title  float  optional
              Title-safe fraction. Defaults to 0.8.
              Unit: fraction of the frame, 0..1
              Range: 0..1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.composition.set-safe-areas","args":{}}]}}
          Python:
            bloom.ops.composition.set_safe_areas()
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.composition.set-safe-areas","args":{}}]

        Related: bloom.composition.set-settings, transact
        """
        ...
    def set_settings(self, name: str = ..., width: int = ..., height: int = ..., pixelAspectNumerator: int = ..., pixelAspectDenominator: int = ..., frameRateNumerator: int = ..., frameRateDenominator: int = ..., duration: Time = ..., start: Time = ..., dropFrame: bool = ..., background: tuple[float, float, float, float] = ..., workingColorSpace: str = ..., actionSafe: float = ..., titleSafe: float = ..., anchor: int = ..., *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.composition.set-settings  (operation, composition)
        Changes the named Composition Settings in one step; omitted settings stay. A size change keeps
        top-level layers pinned to `anchor`.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          name  str  optional
              The new name.
          width  int  optional
              Pixel width.
              Unit: pixels
              Range: 1..1048576
          height  int  optional
              Pixel height.
              Unit: pixels
              Range: 1..1048576
          pixelAspectNumerator  int  optional
              Pixel aspect numerator (with pixelAspectDenominator).
              Unit: pixel aspect as numerator / denominator (1/1 is square)
              Range: >= 1
          pixelAspectDenominator  int  optional
              Pixel aspect denominator (with pixelAspectNumerator).
              Unit: pixel aspect as numerator / denominator (1/1 is square)
              Range: >= 1
          frameRateNumerator  int  optional
              Frame-rate numerator (with frameRateDenominator).
              Unit: frames per second as numerator / denominator (24000/1001 is 23.976)
              Range: 1..4294967295
          frameRateDenominator  int  optional
              Frame-rate denominator (with frameRateNumerator).
              Unit: frames per second as numerator / denominator (24000/1001 is 23.976)
              Range: 1..4294967295
          duration  time  optional
              The duration.
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]
          start  time  optional
              The timecode label of the composition's first frame.
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]
          dropFrame  bool  optional
              Drop-frame timecode labels (honoured at 29.97 and 59.94 fps).
          background  color4  optional
              Background colour.
              Unit: straight (unpremultiplied) RGBA in the composition's working colour space; red,
              green and blue are finite and may exceed 1, alpha is 0..1; display-only: renders, exports
              and captures keep alpha and do not include the background
          workingColorSpace  str  optional
              Working colour space id; an empty string follows the project.
              Unit: a scene-linear colour space name of the project's OCIO config; empty follows the
              project
          actionSafe  float  optional
              Action-safe fraction, 0..1.
              Unit: fraction of the frame, 0..1
              Range: 0..1
          titleSafe  float  optional
              Title-safe fraction, inside action safe.
              Unit: fraction of the frame, 0..1
              Range: 0..1
          anchor  int  optional, default 4 (centre)
              Resize anchor 0..8 (row * 3 + column); 4 keeps content centred.
              Range: 0..8
              Values:
                0  Top left
                1  Top centre
                2  Top right
                3  Middle left
                4  Centre
                5  Middle right
                6  Bottom left
                7  Bottom centre
                8  Bottom right

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: A pair is incomplete (frameRateNumerator without frameRateDenominator, or the pixel
              aspect parts)
              Do: pass both parts of the pair.
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.composition.set-settings","args":{}}]}}
          Python:
            bloom.ops.composition.set_settings()
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.composition.set-settings","args":{}}]

        Related: query:compositions, bloom.composition.set-duration, bloom.composition.set-format,
        transact
        """
        ...
    def set_work_area(self, start: Time, end: Time, *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.composition.set-work-area  (operation, composition)
        Sets the work area: the span playback, RAM preview and work-area export jobs cover. `end` is
        exclusive and at most the composition duration.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          start  time  required
              The work area's first frame.
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]
          end  time  required
              The work area's exclusive end, at most the composition duration.
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.command-rejected
              When: start is not before end, or end is past the duration
              Do: pass 0 <= start < end <= duration (query:compositions reports the duration).
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.composition.set-work-area","args":{"start":0,"end":24}}]}}
          Python:
            bloom.ops.composition.set_work_area(start=0, end=24)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.composition.set-work-area","args":{"start":0,"end":24}}]

        Related: bloom.composition.clear-work-area, query:compositions, transact
        """
        ...

composition: CompositionOps

class DataBlockOps:
    def create(self, typeId: str, mediaType: str, kind: str = ..., payloadKind: str = ..., payload: tuple = ..., text: str = ..., createdAt: str = ..., schemaMajor: int = ..., schemaMinor: int = ..., tags: tuple = ...) -> Mapping[str, Any] | None:
        """bloom.data-block.create  (operation, data-block)
        Creates a data block: a curve, ramp or opaque payload nodes can read.

        Arguments:
          typeId  str  required
              The block's authored type id.
          mediaType  str  required
              The payload media type.
          kind  str  optional, default opaque
              Block kind name, for example "curve", "ramp" or "opaque".
              Values:
                "curve"  Curve
                "ramp"  Ramp
                "opaque"  Opaque
          payloadKind  str  optional, default opaque
              "curve", "ramp" or "opaque". Defaults to "opaque".
              Values:
                "curve"  Curve
                "ramp"  Ramp
                "opaque"  Opaque
          payload  array  optional
              A curve payload as (domainStart, domainEnd, sample, ...), or a ramp payload as (position,
              r, g, b, a, ...).
          text  str  optional
              Opaque payload bytes, as UTF-8 text.
          createdAt  str  optional
              The provenance timestamp.
          schemaMajor  int  optional
              Schema major version.
          schemaMinor  int  optional
              Schema minor version.
          tags  array  optional
              Block tags.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: dataBlock
          (each {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.data-block.create","args":{"typeId":"bloom.curve","mediaType":"application/vnd.bloom.curve"}}]}}
          Python:
            bloom.ops.data_block.create(typeId="bloom.curve", mediaType="application/vnd.bloom.curve")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.data-block.create","args":{"typeId":"bloom.curve","mediaType":"application/vnd.bloom.curve"}}]

        Related: bloom.data-block.replace-payload, transact
        """
        ...
    def relink(self, dataBlock: int | Proxy, path: str, locatorKind: str = ..., portability: str = ..., relinkHint: str = ...) -> Mapping[str, Any] | None:
        """bloom.data-block.relink  (operation, data-block)
        Points a data block at another source file.

        Arguments:
          dataBlock  id  required
              The block to repoint.
              Unit: data block id
              Range: >= 1
          path  str  required
              The replacement source path.
          locatorKind  str  optional
              Locator kind. Defaults to "file".
          portability  str  optional, default project-relative
              Locator portability. Defaults to "project-relative".
              Values:
                "project-relative"  Project-relative file
                "external"  External file
          relinkHint  str  optional
              A relink hint to remember.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.data-block.relink","args":{"dataBlock":1,"path":"data/curve.json"}}]}}
          Python:
            bloom.ops.data_block.relink(dataBlock=1, path="data/curve.json")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.data-block.relink","args":{"dataBlock":1,"path":"data/curve.json"}}]

        Related: transact
        """
        ...
    def remove(self, dataBlock: int | Proxy) -> Mapping[str, Any] | None:
        """bloom.data-block.remove  (operation, data-block)
        Removes a data block.

        Arguments:
          dataBlock  id  required
              The block to remove.
              Unit: data block id
              Range: >= 1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.data-block.remove","args":{"dataBlock":1}}]}}
          Python:
            bloom.ops.data_block.remove(dataBlock=1)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.data-block.remove","args":{"dataBlock":1}}]

        Related: transact
        """
        ...
    def replace_payload(self, dataBlock: int | Proxy, payloadKind: str = ..., payload: tuple = ..., text: str = ...) -> Mapping[str, Any] | None:
        """bloom.data-block.replace-payload  (operation, data-block)
        Replaces a data block's payload.

        Arguments:
          dataBlock  id  required
              The block whose payload is replaced.
              Unit: data block id
              Range: >= 1
          payloadKind  str  optional, default opaque
              "curve", "ramp" or "opaque". Defaults to "opaque".
              Values:
                "curve"  Curve
                "ramp"  Ramp
                "opaque"  Opaque
          payload  array  optional
              Curve or ramp payload numbers.
          text  str  optional
              Opaque payload bytes, as UTF-8 text.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.data-block.replace-payload","args":{"dataBlock":1}}]}}
          Python:
            bloom.ops.data_block.replace_payload(dataBlock=1)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.data-block.replace-payload","args":{"dataBlock":1}}]

        Related: bloom.data-block.create, transact
        """
        ...
    def set_tags(self, dataBlock: int | Proxy, tags: tuple) -> Mapping[str, Any] | None:
        """bloom.data-block.set-tags  (operation, data-block)
        Replaces a data block's tag list.

        Arguments:
          dataBlock  id  required
              The block to tag.
              Unit: data block id
              Range: >= 1
          tags  array  required
              The complete tag list.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.data-block.set-tags","args":{"dataBlock":1,"tags":["authored"]}}]}}
          Python:
            bloom.ops.data_block.set_tags(dataBlock=1, tags=("authored",))
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.data-block.set-tags","args":{"dataBlock":1,"tags":["authored"]}}]

        Related: transact
        """
        ...

data_block: DataBlockOps

class ExportOps:
    def add_job(self, name: str = ..., range: str = ..., first: int = ..., last: int = ..., enabled: bool = ..., labelColor: tuple = ..., outputs: tuple = ..., *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.export.add-job  (operation, export)
        Adds an export job for a composition to the project's export queue (the Export panel's Add).
        Output keeps alpha; the composition background colour is not rendered. Start it with the
        export.start tool.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to export. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          name  str  optional
              The job's name.
          range  str  optional, default work-area
              "work-area" (default), "composition", "custom" or "frame".
              Values:
                "work-area"  Work area
                "composition"  Whole composition
                "custom"  Custom range first..last (inclusive)
                "frame"  One frame (first)
          first  int  optional
              First frame of a custom range, or the frame of a frame range.
              Unit: composition frame index (inclusive)
              Range: >= 0
          last  int  optional
              Last frame of a custom range.
              Unit: composition frame index (inclusive)
              Range: >= 0
          enabled  bool  optional
              Included by Start All.
          labelColor  array  optional
              Optional row label as three integers 0..255.
              Unit: label colour as [r, g, b], each an integer 0..255
              Range: 0..255
          outputs  array  optional
              Outputs as JSON strings; omitted adds one H.264 MP4 output. One output as JSON: {"preset":
              id, "settings": {document 1.29 settings}, "destination": {"portability", "path",
              "overwrite"}, "enabled", "importIntoAssets"}.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: exportJob,
          exportOutput (one per output) (each {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.export.add-job","args":{}}]}}
          Python:
            bloom.ops.export.add_job()
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.export.add-job","args":{}}]

        Related: export.start, export.status, export.presets, bloom.export.add-output, transact
        """
        ...
    def add_output(self, job: int | Proxy, output: str) -> Mapping[str, Any] | None:
        """bloom.export.add-output  (operation, export)
        Adds an output (a preset, settings and destination) to an export job.

        Arguments:
          job  id  required
              The job.
              Unit: export job id (export.status)
              Range: >= 1
          output  str  required
              One output as JSON: {"preset": id, "settings": {document 1.29 settings}, "destination":
              {"portability", "path", "overwrite"}, "enabled", "importIntoAssets"}.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: exportOutput
          (each {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.export.add-output","args":{"job":1,"output":"{}"}}]}}
          Python:
            bloom.ops.export.add_output(job=1, output="{}")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.export.add-output","args":{"job":1,"output":"{}"}}]

        Related: export.presets, bloom.export.set-output-destination, transact
        """
        ...
    def apply_preset(self, output: int | Proxy, preset: str) -> Mapping[str, Any] | None:
        """bloom.export.apply-preset  (operation, export)
        Replaces an output's settings with a built-in preset's.

        Arguments:
          output  id  required
              The output.
              Unit: export output id (export.status)
              Range: >= 1
          preset  str  required
              A built-in preset id (export.presets lists them).

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.export.apply-preset","args":{"output":1,"preset":"builtin:prores-422hq-mov"}}]}}
          Python:
            bloom.ops.export.apply_preset(output=1, preset="builtin:prores-422hq-mov")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.export.apply-preset","args":{"output":1,"preset":"builtin:prores-422hq-mov"}}]

        Related: export.presets, transact
        """
        ...
    def duplicate_job(self, job: int | Proxy) -> Mapping[str, Any] | None:
        """bloom.export.duplicate-job  (operation, export)
        Copies an export job with its outputs.

        Arguments:
          job  id  required
              The job to copy.
              Unit: export job id (export.status)
              Range: >= 1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: exportJob,
          exportOutput (each {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.export.duplicate-job","args":{"job":1}}]}}
          Python:
            bloom.ops.export.duplicate_job(job=1)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.export.duplicate-job","args":{"job":1}}]

        Related: export.status, transact
        """
        ...
    def move_jobs(self, jobs: tuple, index: int) -> Mapping[str, Any] | None:
        """bloom.export.move-jobs  (operation, export)
        Moves export jobs in the queue's run order.

        Arguments:
          jobs  array  required
              The jobs.
          index  int  required
              Where the first moved job lands in run order.
              Unit: zero-based position
              Range: >= 0

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.export.move-jobs","args":{"jobs":[1],"index":0}}]}}
          Python:
            bloom.ops.export.move_jobs(jobs=(1,), index=0)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.export.move-jobs","args":{"jobs":[1],"index":0}}]

        Related: export.status, transact
        """
        ...
    def remove_finished_jobs(self, jobs: tuple) -> Mapping[str, Any] | None:
        """bloom.export.remove-finished-jobs  (operation, export)
        Removes finished (Done) export jobs.

        Arguments:
          jobs  array  required
              The finished jobs to remove (export.status lists Done jobs).

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.export.remove-finished-jobs","args":{"jobs":[1]}}]}}
          Python:
            bloom.ops.export.remove_finished_jobs(jobs=(1,))
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.export.remove-finished-jobs","args":{"jobs":[1]}}]

        Related: export.status, transact
        """
        ...
    def remove_jobs(self, jobs: tuple) -> Mapping[str, Any] | None:
        """bloom.export.remove-jobs  (operation, export)
        Removes export jobs from the queue.

        Arguments:
          jobs  array  required
              The jobs.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.export.remove-jobs","args":{"jobs":[1]}}]}}
          Python:
            bloom.ops.export.remove_jobs(jobs=(1,))
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.export.remove-jobs","args":{"jobs":[1]}}]

        Related: export.status, transact
        """
        ...
    def remove_output(self, output: int | Proxy) -> Mapping[str, Any] | None:
        """bloom.export.remove-output  (operation, export)
        Removes an output from its job.

        Arguments:
          output  id  required
              The output.
              Unit: export output id (export.status)
              Range: >= 1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.export.remove-output","args":{"output":1}}]}}
          Python:
            bloom.ops.export.remove_output(output=1)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.export.remove-output","args":{"output":1}}]

        Related: transact
        """
        ...
    def set_job_enabled(self, job: int | Proxy, enabled: bool) -> Mapping[str, Any] | None:
        """bloom.export.set-job-enabled  (operation, export)
        Includes or excludes an export job from export.start-all.

        Arguments:
          job  id  required
              The job.
              Unit: export job id (export.status)
              Range: >= 1
          enabled  bool  required
              Included by Start All.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.export.set-job-enabled","args":{"job":1,"enabled":true}}]}}
          Python:
            bloom.ops.export.set_job_enabled(job=1, enabled=True)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.export.set-job-enabled","args":{"job":1,"enabled":true}}]

        Related: export.start-all, transact
        """
        ...
    def set_job_label_color(self, job: int | Proxy, labelColor: tuple = ...) -> Mapping[str, Any] | None:
        """bloom.export.set-job-label-color  (operation, export)
        Sets or clears a job's row label colour.

        Arguments:
          job  id  required
              The job.
              Unit: export job id (export.status)
              Range: >= 1
          labelColor  array  optional
              Three integers 0..255; omitted clears the label.
              Unit: label colour as [r, g, b], each an integer 0..255
              Range: 0..255

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.export.set-job-label-color","args":{"job":1}}]}}
          Python:
            bloom.ops.export.set_job_label_color(job=1)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.export.set-job-label-color","args":{"job":1}}]

        Related: transact
        """
        ...
    def set_job_name(self, job: int | Proxy, name: str) -> Mapping[str, Any] | None:
        """bloom.export.set-job-name  (operation, export)
        Renames an export job.

        Arguments:
          job  id  required
              The job.
              Unit: export job id (export.status)
              Range: >= 1
          name  str  required
              Empty shows the composition name.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.export.set-job-name","args":{"job":1,"name":"Review"}}]}}
          Python:
            bloom.ops.export.set_job_name(job=1, name="Review")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.export.set-job-name","args":{"job":1,"name":"Review"}}]

        Related: export.status, transact
        """
        ...
    def set_job_range(self, job: int | Proxy, range: str, first: int = ..., last: int = ...) -> Mapping[str, Any] | None:
        """bloom.export.set-job-range  (operation, export)
        Sets which frames an export job renders. Custom ranges are inclusive composition frame indices.

        Arguments:
          job  id  required
              The job.
              Unit: export job id (export.status)
              Range: >= 1
          range  str  required
              The range kind.
              Values:
                "work-area"  Work area
                "composition"  Whole composition
                "custom"  Custom range first..last (inclusive)
                "frame"  One frame (first)
          first  int  optional
              First frame.
              Unit: composition frame index (inclusive)
              Range: >= 0
          last  int  optional
              Last frame (custom).
              Unit: composition frame index (inclusive)
              Range: >= 0

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.export.set-job-range","args":{"job":1,"range":"custom"}}]}}
          Python:
            bloom.ops.export.set_job_range(job=1, range="custom")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.export.set-job-range","args":{"job":1,"range":"custom"}}]

        Related: export.status, transact
        """
        ...
    def set_output_destination(self, output: int | Proxy, path: str, portability: str = ..., overwrite: str = ...) -> Mapping[str, Any] | None:
        """bloom.export.set-output-destination  (operation, export)
        Sets where an output is written: a project-relative path, or a file URI with portability
        external. It may use {name} (composition name), {job} (job name), {format} and {date}; an image
        sequence's pattern also takes <base>, <ext> and exactly one #### or {frame}.

        Arguments:
          output  id  required
              The output.
              Unit: export output id (export.status)
              Range: >= 1
          path  str  required
              Project-relative path, or a file URI with portability external.
          portability  str  optional, default project-relative
              "project-relative" (default) or "external".
              Values:
                "project-relative"  Inside the project folder
                "external"  A file URI anywhere
          overwrite  str  optional, default ask
              "ask" (default), "replace" or "refuse".
              Values:
                "ask"  Ask before replacing (the run stops at the existing file until export.confirm-replace)
                "replace"  Replace the existing file
                "refuse"  Fail when the file exists

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.export.set-output-destination","args":{"output":1,"path":"Exports/{name}.mp4"}}]}}
          Python:
            bloom.ops.export.set_output_destination(output=1, path="Exports/{name}.mp4")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.export.set-output-destination","args":{"output":1,"path":"Exports/{name}.mp4"}}]

        Related: export.confirm-replace, transact
        """
        ...
    def set_output_enabled(self, output: int | Proxy, enabled: bool) -> Mapping[str, Any] | None:
        """bloom.export.set-output-enabled  (operation, export)
        Includes or excludes one output of a job.

        Arguments:
          output  id  required
              The output.
              Unit: export output id (export.status)
              Range: >= 1
          enabled  bool  required
              Render this output.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.export.set-output-enabled","args":{"output":1,"enabled":true}}]}}
          Python:
            bloom.ops.export.set_output_enabled(output=1, enabled=True)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.export.set-output-enabled","args":{"output":1,"enabled":true}}]

        Related: transact
        """
        ...
    def set_output_import(self, output: int | Proxy, importIntoAssets: bool) -> Mapping[str, Any] | None:
        """bloom.export.set-output-import  (operation, export)
        Chooses whether the verified result is imported into Assets.

        Arguments:
          output  id  required
              The output.
              Unit: export output id (export.status)
              Range: >= 1
          importIntoAssets  bool  required
              Import the verified result into Assets.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.export.set-output-import","args":{"output":1,"importIntoAssets":true}}]}}
          Python:
            bloom.ops.export.set_output_import(output=1, importIntoAssets=True)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.export.set-output-import","args":{"output":1,"importIntoAssets":true}}]

        Related: transact
        """
        ...
    def set_output_settings(self, output: int | Proxy, settings: str) -> Mapping[str, Any] | None:
        """bloom.export.set-output-settings  (operation, export)
        Replaces an output's settings with a document 1.29 settings object.

        Arguments:
          output  id  required
              The output.
              Unit: export output id (export.status)
              Range: >= 1
          settings  str  required
              The document 1.29 settings object as JSON.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.export.set-output-settings","args":{"output":1,"settings":"{\\"format\\":\\"h264-mp4\\",\\"codecProfile\\":\\"\\",\\"compression\\":\\"\\",\\"outputColorSpace\\":\\"\\",\\"display\\":\\"\\",\\"view\\":\\"\\",\\"bakeLook\\":true,\\"includeAudio\\":true,\\"useCachedFrames\\":true,\\"audioSampleRate\\":48000,\\"encoder\\":\\"automatic\\",\\"naming\\":{\\"framePadding\\":4,\\"pattern\\":\\"<base>.####.<ext>\\"}}"}}]}}
          Python:
            bloom.ops.export.set_output_settings(output=1, settings="{"format":"h264-mp4","codecProfile":"","compression":"","outputColorSpace":"","display":"","view":"","bakeLook":true,"includeAudio":true,"useCachedFrames":true,"audioSampleRate":48000,"encoder":"automatic","naming":{"framePadding":4,"pattern":"<base>.####.<ext>"}}")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.export.set-output-settings","args":{"output":1,"settings":"{\\"format\\":\\"h264-mp4\\",\\"codecProfile\\":\\"\\",\\"compression\\":\\"\\",\\"outputColorSpace\\":\\"\\",\\"display\\":\\"\\",\\"view\\":\\"\\",\\"bakeLook\\":true,\\"includeAudio\\":true,\\"useCachedFrames\\":true,\\"audioSampleRate\\":48000,\\"encoder\\":\\"automatic\\",\\"naming\\":{\\"framePadding\\":4,\\"pattern\\":\\"<base>.####.<ext>\\"}}"}}]

        Related: bloom.export.apply-preset, export.presets, transact
        """
        ...

export: ExportOps

class LayerOps:
    def add_audio(self, asset: int | Proxy, *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.layer.add-audio  (operation, layer)
        Adds an audio layer for an imported audio asset on top of the layer stack.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          asset  id  required
              The audio asset to place.
              Unit: asset id (query:assets)
              Range: >= 1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: layer, slot,
          audioNode, layerOutputNode (each {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.add-audio","args":{"asset":1}}]}}
          Python:
            bloom.ops.layer.add_audio(asset=1)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.add-audio","args":{"asset":1}}]

        Related: bloom.asset.import, query:layers, transact
        """
        ...
    def add_from_assets(self, assets: tuple = ..., compositions: tuple = ..., placement: str = ..., before: int | Proxy = ..., sequence: bool = ..., *, composition: int | Proxy = ..., time: Time = ...) -> Mapping[str, Any] | None:
        """bloom.layer.add-from-assets  (operation, layer)
        Adds footage and compositions as layers, as dropping them from Assets on the Timeline or the
        Viewer does (ADR 0030), in ONE undo step. Each asset becomes the layer for its kind: an image,
        image sequence or video asset an image/video layer, an audio asset an audio layer; each
        composition a nested-composition layer named after it. Every layer keeps its media's own length
        (ADR 0024: after its last frame it shows nothing). Order: the assets in the order given, then
        the compositions; the first item is the uppermost layer. Placement: all new layers go on top of
        the stack (placement "top", the default), below every layer ("bottom"), or directly above the
        layer `before` ("before"). The first layer starts at `time` snapped to the nearest composition
        frame; with sequence=true each next layer starts where the one before ends (Alt-drop, After
        Effects' Sequence Layers), otherwise all start at `time`. Side effects: adds a source node and a
        Layer Output node per item to the composition graph and a stack slot on the output Merge; nested
        layers also route audio. Does not import files: run bloom.asset.import first and pass the asset
        ids it outputs. Outputs: one "layer" output per item, in item order (layer ids). Errors: "This
        item cannot be a layer here" - an id that is not an image, sequence, video or audio asset (fonts
        and LUTs are not layers), a missing asset or composition, or the target composition itself; pass
        only footage and other compositions. "Nothing to add" - both arrays empty. "The layer to insert
        above no longer exists" - `before` is not a layer of this composition. A composition that would
        nest itself (a cycle) is rejected. Undo: one step, the transaction label (the UI uses "Add
        Layer", "Add Composition Layer" or "Add N Layers"). MCP example:
        {"op":"bloom.layer.add-from-assets","args":{"assets":[3,4],"time":0,"sequence":true}}.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              Id. The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          time  time  optional, defaults to the playhead (frame 0 headless) (context time)
              Time: a whole frame of the composition at its frame rate, or an exact [numerator,
              denominator] time in seconds). Where the first layer starts, snapped to the nearest frame
              inside the composition. Defaults to the playhead.
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]
          assets  array  optional
              Array of asset ids: image, image sequence, video or audio assets, in stack order (first is
              uppermost). Default: none.
          compositions  array  optional
              Array of composition ids to nest, after the assets. Not the target composition. Default:
              none.
          placement  str  optional, default top
              Enum "top" (default: above every layer), "bottom" (below every layer) or "before"
              (directly above the layer `before`).
              Values:
                "top"  Above every layer
                "bottom"  Below every layer
                "before"  Directly above the layer `before`
          before  id  optional
              Id. The layer the new layers go directly above. Required with placement "before"; giving
              it alone implies "before".
              Unit: layer id (query:layers .id)
              Range: >= 1
          sequence  bool  optional
              Boolean. true: each layer starts where the previous one ends. Default false.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.add-from-assets","args":{"assets":[1]}}]}}
          Python:
            bloom.ops.layer.add_from_assets(assets=(1,))
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.add-from-assets","args":{"assets":[1]}}]

        Related: transact
        """
        ...
    def add_image(self, asset: int | Proxy, *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.layer.add-image  (operation, layer)
        Adds a layer showing an imported image, image sequence or video asset on top of the layer stack,
        centred in the composition. A still image spans the composition; a sequence or video starts at
        frame 0 and lasts its natural length (capped by the composition), showing nothing after its last
        frame.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          asset  id  required
              The image asset to place.
              Unit: asset id (query:assets)
              Range: >= 1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: layer, slot,
          imageNode, layerOutputNode, positionParameter, anchorParameter, scaleParameter,
          rotationParameter, opacityParameter, blendModeParameter, imageToLayerEdge, layerToStackEdge
          (each {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.add-image","args":{"asset":1}}]}}
          Python:
            bloom.ops.layer.add_image(asset=1)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.add-image","args":{"asset":1}}]

        Related: bloom.asset.import, query:layers, transact
        """
        ...
    def add_mask(self, path: Path, mode: int = ..., name: str = ..., *, composition: int | Proxy = ..., layer: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.layer.add-mask  (operation, layer)
        Adds a mask to a layer. The path is in the layer's content space (pixels of the layer's own
        content, before its transform).

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          layer  id  optional, defaults to the first selected layer (context layer)
              The layer to edit: a layer id, not its node's id. Defaults to the first selected layer.
              Unit: layer id (query:layers .id)
              Range: >= 1
          path  path  required
              The mask outline in the layer's content space (closed to cover an area).
              Unit: {closed, anchors:[{point:[x, y], inHandle:[x, y]|null, outHandle:[x, y]|null,
              mode?}]} in the layer's content-space pixels; handles are relative to their anchor point
          mode  int  optional, default 1 (Add)
              0 None, 1 Add, 2 Subtract, 3 Intersect, 4 Difference. Defaults to Add.
              Range: 0..4
              Values:
                0  None
                1  Add
                2  Subtract
                3  Intersect
                4  Difference
          name  str  optional
              The mask name. Defaults to Mask N.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: mask, path,
          feather, expansion, opacity (each {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.add-mask","args":{"path":{"closed":true,"anchors":[{"point":[0,0],"inHandle":null,"outHandle":null},{"point":[100,0],"inHandle":null,"outHandle":null},{"point":[100,100],"inHandle":null,"outHandle":null},{"point":[0,100],"inHandle":null,"outHandle":null}]}}}]}}
          Python:
            bloom.ops.layer.add_mask(path={"closed": True, "anchors": [{"point": (0, 0), "in_handle": None, "out_handle": None}, {"point": (100, 0), "in_handle": None, "out_handle": None}, {"point": (100, 100), "in_handle": None, "out_handle": None}, {"point": (0, 100), "in_handle": None, "out_handle": None}]})
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.add-mask","args":{"path":{"closed":true,"anchors":[{"point":[0,0],"inHandle":null,"outHandle":null},{"point":[100,0],"inHandle":null,"outHandle":null},{"point":[100,100],"inHandle":null,"outHandle":null},{"point":[0,100],"inHandle":null,"outHandle":null}]}}}]

        Related: bloom.layer.set-mask-path, query:layers, transact
        """
        ...
    def add_shape(self, kind: str, position: tuple[float, float] = ..., size: tuple[float, float] = ..., lineStart: tuple[float, float] = ..., lineEnd: tuple[float, float] = ..., path: tuple = ..., pathClosed: bool = ..., *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.layer.add-shape  (operation, layer)
        Adds a shape layer (rectangle, ellipse, triangle, polygon, star, line or a path) on top of the
        layer stack. Fill, stroke and other shape parameters are edited afterwards with
        bloom.parameter.set-source (query:layers lists them).

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          kind  str  required
              "rectangle", "ellipse", "triangle", "polygon", "star", "line" or "path".
              Values:
                "rectangle"  Rectangle
                "ellipse"  Ellipse
                "triangle"  Triangle
                "polygon"  Polygon
                "star"  Star
                "line"  Line
                "path"  Path
          position  vec2  optional, default the composition centre
              Layer position. Defaults to the composition centre.
              Unit: composition pixels: x to the right, y down, origin at the frame's top-left corner
          size  vec2  optional, default [400, 300]
              Shape size.
              Unit: pixels as [width, height]
          lineStart  vec2  optional
              First point of a line shape.
              Unit: composition pixels: x to the right, y down, origin at the frame's top-left corner
          lineEnd  vec2  optional
              Second point of a line shape.
              Unit: composition pixels: x to the right, y down, origin at the frame's top-left corner
          path  array  optional
              Path anchors as (x, y) or (x, y, inX, inY, outX, outY).
          pathClosed  bool  optional
              Whether a path shape is closed.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: layer, slot,
          layerOutputNode and the shape's nodes and parameters (listed in the result) (each
          {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.add-shape","args":{"kind":"rectangle"}}]}}
          Python:
            bloom.ops.layer.add_shape(kind="rectangle")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.add-shape","args":{"kind":"rectangle"}}]

        Related: query:layers, bloom.parameter.set-source, transact
        """
        ...
    def add_solid(self, name: str, color: tuple[float, float, float, float], position: tuple[float, float] = ..., opacity: float = ..., *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.layer.add-solid  (operation, layer)
        Adds a composition-sized solid colour layer on top of the layer stack.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          name  str  required
              The layer name.
          color  color4  required
              The solid colour as (r, g, b, a).
              Unit: straight (unpremultiplied) RGBA in the composition's working colour space; red,
              green and blue are finite and may exceed 1, alpha is 0..1
          position  vec2  optional, default the composition centre
              Layer position. Defaults to the composition centre.
              Unit: composition pixels: x to the right, y down, origin at the frame's top-left corner
          opacity  float  optional, default 1
              Layer opacity in 0..1. Defaults to 1.
              Unit: fraction 0..1
              Range: 0..1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: layer, slot,
          solidNode, layerOutputNode, colorParameter, widthParameter, heightParameter,
          positionParameter, anchorParameter, scaleParameter, rotationParameter, opacityParameter,
          blendModeParameter (each {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.add-solid","args":{"name":"Red","color":[1,0,0,1]}}]}}
          Python:
            bloom.ops.layer.add_solid(name="Red", color=(1, 0, 0, 1))
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.add-solid","args":{"name":"Red","color":[1,0,0,1]}}]

        Related: query:layers, bloom.parameter.set-source,
        bloom.animation.set-keyframe-at-time-for-parameter, transact
        """
        ...
    def add_text(self, name: str, text: str, position: tuple[float, float] = ..., opacity: float = ..., size: float = ..., color: tuple[float, float, float, float] = ..., *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.layer.add-text  (operation, layer)
        Adds a text layer on top of the layer stack. Font, alignment, box and the other text parameters
        are edited afterwards with bloom.parameter.set-source; their ids are in the outputs and in
        query:layers.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          name  str  required
              The layer name.
          text  str  required
              The text content.
          position  vec2  optional, default the composition centre
              Layer position. Defaults to the composition centre.
              Unit: composition pixels: x to the right, y down, origin at the frame's top-left corner
          opacity  float  optional, default 1
              Layer opacity in 0..1. Defaults to 1.
              Unit: fraction 0..1
              Range: 0..1
          size  float  optional, default 72
              Em size in pixels.
              Unit: pixels (em size)
              Range: >= 0
          color  color4  optional
              Fill colour. Defaults to opaque white.
              Unit: straight (unpremultiplied) RGBA in the composition's working colour space; red,
              green and blue are finite and may exceed 1, alpha is 0..1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: layer, slot,
          textNode, layerOutputNode, textParameter, sizeParameter, colorParameter, fontParameter,
          boxParameter, wrapParameter, anchorModeParameter, overflowParameter, positionParameter,
          anchorParameter, scaleParameter, rotationParameter, opacityParameter, blendModeParameter (each
          {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.add-text","args":{"name":"Title","text":"Hello, Bloom!"}}]}}
          Python:
            bloom.ops.layer.add_text(name="Title", text="Hello, Bloom!")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.add-text","args":{"name":"Title","text":"Hello, Bloom!"}}]

        Related: query:layers, bloom.parameter.set-source, transact
        """
        ...
    def align_layers(self, edge: str, target: str = ..., key: int | Proxy = ..., *, composition: int | Proxy = ..., layers: tuple = ..., time: Time = ...) -> Mapping[str, Any] | None:
        """bloom.layer.align-layers  (operation, layer)
        Lines up layers' evaluated bounds at `time` (Composition > Align). The bounds are measured on
        the server before the transaction runs; skipped layers are warnings.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          edge  str  required
              The edge or centre to line up: "left", "horizontal-centre", "right", "top",
              "vertical-middle" or "bottom".
              Values:
                "left"  Left
                "horizontal-centre"  Horizontal centre
                "right"  Right
                "top"  Top
                "vertical-middle"  Vertical middle
                "bottom"  Bottom
          target  str  optional, default selection
              What to line up with: "selection" (the union of the layers' bounds), "composition" (the
              composition frame) or "key-object" (the key layer's bounds). Defaults to "selection".
              Values:
                "selection"  The union of the layers' bounds
                "composition"  The composition frame
                "key-object"  The key layer's bounds
          key  id  optional
              The key layer for target "key-object". It never moves.
              Unit: layer id (query:layers .id)
              Range: >= 1
          layers  array  optional, defaults to the selected layers (context layers)
              The layers to move. Defaults to the selected layers.
              Unit: array of layer ids (query:layers .id); defaults to the selected layers
          time  time  optional, defaults to the playhead (frame 0 headless) (context time)
              The time whose evaluated bounds are aligned. Defaults to the current playhead time.
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: movedLayer,
          skippedLayer (each {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.align-layers","args":{"edge":"left"}}]}}
          Python:
            bloom.ops.layer.align_layers(edge="left")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.align-layers","args":{"edge":"left"}}]

        Related: bloom.layer.distribute-layers, query:layers, transact
        """
        ...
    def animate_mask_path(self, mask: int | Proxy, *, composition: int | Proxy = ..., layer: int | Proxy = ..., time: Time = ...) -> Mapping[str, Any] | None:
        """bloom.layer.animate-mask-path  (operation, layer)
        Makes a mask path animated with a key at `time`.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          layer  id  optional, defaults to the first selected layer (context layer)
              The layer to edit: a layer id, not its node's id. Defaults to the first selected layer.
              Unit: layer id (query:layers .id)
              Range: >= 1
          mask  id  required
              The layer mask to edit.
              Unit: mask id (query:layers .masks)
              Range: >= 1
          time  time  optional, defaults to the playhead (frame 0 headless) (context time)
              An exact time. Defaults to the current playhead time.
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: curve,
          keyframe (each {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.animate-mask-path","args":{"mask":1}}]}}
          Python:
            bloom.ops.layer.animate_mask_path(mask=1)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.animate-mask-path","args":{"mask":1}}]

        Related: bloom.layer.set-mask-path, bloom.layer.stop-animating-mask-path, transact
        """
        ...
    def clear_track_matte(self, *, composition: int | Proxy = ..., layer: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.layer.clear-track-matte  (operation, layer)
        Removes a layer's track matte.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          layer  id  optional, defaults to the first selected layer (context layer)
              The layer to edit: a layer id, not its node's id. Defaults to the first selected layer.
              Unit: layer id (query:layers .id)
              Range: >= 1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.clear-track-matte","args":{}}]}}
          Python:
            bloom.ops.layer.clear_track_matte()
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.clear-track-matte","args":{}}]

        Related: bloom.layer.set-track-matte, transact
        """
        ...
    def delete(self, *, composition: int | Proxy = ..., layers: tuple = ...) -> Mapping[str, Any] | None:
        """bloom.layer.delete  (operation, layer)
        Deletes layers, as Delete in the Timeline or Delete Layer in the Viewer's context menu does:
        each layer's Layer Output node is removed with its stack slot, masks, track-matte and parent
        links. Upstream source nodes are left in the graph, as in the UI. Outputs: none. Errors: "Layer
        N is not in the composition" - a stale or foreign layer id, re-read the layers; "No layers were
        named" - an empty array with no selected layers. Undo: one step; undo restores the layers with
        their ids. MCP example: {"op":"bloom.layer.delete","args":{"layers":[2]}}.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              Id. The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          layers  array  optional, defaults to the selected layers (context layers)
              Array of layer ids (the ids query kind=layers and bloom.layer.add-* outputs return; not
              node ids). Defaults to every selected layer.
              Unit: array of layer ids (query:layers .id); defaults to the selected layers

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.delete","args":{}}]}}
          Python:
            bloom.ops.layer.delete()
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.delete","args":{}}]

        Related: transact
        """
        ...
    def disable_time_remap(self, *, composition: int | Proxy = ..., layer: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.layer.disable-time-remap  (operation, layer)
        Turns off Time Remap for a layer.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          layer  id  optional, defaults to the first selected layer (context layer)
              The layer to edit: a layer id, not its node's id. Defaults to the first selected layer.
              Unit: layer id (query:layers .id)
              Range: >= 1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.disable-time-remap","args":{}}]}}
          Python:
            bloom.ops.layer.disable_time_remap()
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.disable-time-remap","args":{}}]

        Related: bloom.layer.enable-time-remap, transact
        """
        ...
    def distribute_layers(self, axis: str, target: str = ..., key: int | Proxy = ..., *, composition: int | Proxy = ..., layers: tuple = ..., time: Time = ...) -> Mapping[str, Any] | None:
        """bloom.layer.distribute-layers  (operation, layer)
        Spaces three or more layers' evaluated centres evenly (Composition > Distribute).

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          axis  str  required
              Space the layers' centres evenly along "horizontal" or "vertical". Needs three or more
              layers.
              Values:
                "horizontal"  Horizontal
                "vertical"  Vertical
          target  str  optional, default selection
              What to line up with: "selection" (the union of the layers' bounds), "composition" (the
              composition frame) or "key-object" (the key layer's bounds). Defaults to "selection".
              Values:
                "selection"  The union of the layers' bounds
                "composition"  The composition frame
                "key-object"  The key layer's bounds
          key  id  optional
              The key layer for target "key-object". It never moves.
              Unit: layer id (query:layers .id)
              Range: >= 1
          layers  array  optional, defaults to the selected layers (context layers)
              The layers to move. Defaults to the selected layers.
              Unit: array of layer ids (query:layers .id); defaults to the selected layers
          time  time  optional, defaults to the playhead (frame 0 headless) (context time)
              The time whose evaluated bounds are aligned. Defaults to the current playhead time.
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: movedLayer,
          skippedLayer (each {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.distribute-layers","args":{"axis":"horizontal"}}]}}
          Python:
            bloom.ops.layer.distribute_layers(axis="horizontal")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.distribute-layers","args":{"axis":"horizontal"}}]

        Related: bloom.layer.align-layers, query:layers, transact
        """
        ...
    def duplicate(self, *, composition: int | Proxy = ..., layers: tuple = ...) -> Mapping[str, Any] | None:
        """bloom.layer.duplicate  (operation, layer)
        Duplicates layers, as Duplicate Layer in the Viewer's context menu does: each layer's Layer
        Output node is copied with its parameters, keyframes, masks, timing, parent and matte, reading
        the same source as the original, and the copy takes a new stack slot directly below the
        original. Layers duplicated together keep their parent and matte links among the copies.
        Outputs: one "layer" output per copy in argument order, plus "layer.<original>" -> copy,
        "node.<original>" -> copied node and "slot.<original>" -> new slot. Errors: "Layer N is not in
        the composition" - a stale or foreign layer id; "Driven parameters cannot be deeply duplicated"
        - a parameter driven by a node link. Undo: one step. MCP example:
        {"op":"bloom.layer.duplicate","args":{"layers":[2]}}.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              Id. The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          layers  array  optional, defaults to the selected layers (context layers)
              Array of layer ids (the ids query kind=layers and bloom.layer.add-* outputs return; not
              node ids). Defaults to every selected layer.
              Unit: array of layer ids (query:layers .id); defaults to the selected layers

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.duplicate","args":{}}]}}
          Python:
            bloom.ops.layer.duplicate()
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.duplicate","args":{}}]

        Related: transact
        """
        ...
    def enable_time_remap(self, *, composition: int | Proxy = ..., layer: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.layer.enable-time-remap  (operation, layer)
        Turns on Time Remap for a layer: an animatable parameter mapping layer time to content time.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          layer  id  optional, defaults to the first selected layer (context layer)
              The layer to edit: a layer id, not its node's id. Defaults to the first selected layer.
              Unit: layer id (query:layers .id)
              Range: >= 1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.enable-time-remap","args":{}}]}}
          Python:
            bloom.ops.layer.enable_time_remap()
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.enable-time-remap","args":{}}]

        Related: bloom.layer.disable-time-remap, query:layers, transact
        """
        ...
    def move_before(self, slot: int | Proxy, before: int | Proxy = ..., *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.layer.move-before  (operation, layer)
        Reorders the layer stack: moves a slot to sit above another slot, or to the bottom. query:layers
        reports each layer's slot; index 0 is the topmost layer.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          slot  id  required
              The stack slot to move.
              Unit: layer-stack slot id (query:layers .slot)
              Range: >= 1
          before  id  optional
              The slot to land above. Omit to move to the bottom.
              Unit: layer-stack slot id (query:layers .slot)
              Range: >= 1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.move-before","args":{"slot":1}}]}}
          Python:
            bloom.ops.layer.move_before(slot=1)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.move-before","args":{"slot":1}}]

        Related: query:layers, bloom.merge.reorder-input, transact
        """
        ...
    def move_mask(self, mask: int | Proxy, index: int, *, composition: int | Proxy = ..., layer: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.layer.move-mask  (operation, layer)
        Moves a mask in its layer's mask order.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          layer  id  optional, defaults to the first selected layer (context layer)
              The layer to edit: a layer id, not its node's id. Defaults to the first selected layer.
              Unit: layer id (query:layers .id)
              Range: >= 1
          mask  id  required
              The layer mask to edit.
              Unit: mask id (query:layers .masks)
              Range: >= 1
          index  int  required
              The new zero-based position.
              Unit: zero-based position
              Range: >= 0

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.move-mask","args":{"mask":1,"index":0}}]}}
          Python:
            bloom.ops.layer.move_mask(mask=1, index=0)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.move-mask","args":{"mask":1,"index":0}}]

        Related: query:layers, transact
        """
        ...
    def remove_mask(self, mask: int | Proxy, *, composition: int | Proxy = ..., layer: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.layer.remove-mask  (operation, layer)
        Removes a mask from a layer.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          layer  id  optional, defaults to the first selected layer (context layer)
              The layer to edit: a layer id, not its node's id. Defaults to the first selected layer.
              Unit: layer id (query:layers .id)
              Range: >= 1
          mask  id  required
              The layer mask to edit.
              Unit: mask id (query:layers .masks)
              Range: >= 1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.remove-mask","args":{"mask":1}}]}}
          Python:
            bloom.ops.layer.remove_mask(mask=1)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.remove-mask","args":{"mask":1}}]

        Related: query:layers, transact
        """
        ...
    def rename(self, name: str, *, composition: int | Proxy = ..., layer: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.layer.rename  (operation, layer)
        Renames a layer.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          layer  id  optional, defaults to the first selected layer (context layer)
              The layer to edit: a layer id, not its node's id. Defaults to the first selected layer.
              Unit: layer id (query:layers .id)
              Range: >= 1
          name  str  required
              The new layer name.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.rename","args":{"name":"Title"}}]}}
          Python:
            bloom.ops.layer.rename(name="Title")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.rename","args":{"name":"Title"}}]

        Related: query:layers, transact
        """
        ...
    def rename_mask(self, mask: int | Proxy, name: str, *, composition: int | Proxy = ..., layer: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.layer.rename-mask  (operation, layer)
        Renames a mask.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          layer  id  optional, defaults to the first selected layer (context layer)
              The layer to edit: a layer id, not its node's id. Defaults to the first selected layer.
              Unit: layer id (query:layers .id)
              Range: >= 1
          mask  id  required
              The layer mask to edit.
              Unit: mask id (query:layers .masks)
              Range: >= 1
          name  str  required
              The new mask name.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.rename-mask","args":{"mask":1,"name":"Mask 1"}}]}}
          Python:
            bloom.ops.layer.rename_mask(mask=1, name="Mask 1")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.rename-mask","args":{"mask":1,"name":"Mask 1"}}]

        Related: query:layers, transact
        """
        ...
    def set_enabled(self, enabled: bool, *, composition: int | Proxy = ..., layer: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.layer.set-enabled  (operation, layer)
        Shows or hides a layer (the Timeline eye).

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          layer  id  optional, defaults to the first selected layer (context layer)
              The layer to edit: a layer id, not its node's id. Defaults to the first selected layer.
              Unit: layer id (query:layers .id)
              Range: >= 1
          enabled  bool  required
              Whether the layer renders.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.set-enabled","args":{"enabled":true}}]}}
          Python:
            bloom.ops.layer.set_enabled(enabled=True)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.set-enabled","args":{"enabled":true}}]

        Related: query:layers, transact
        """
        ...
    def set_label_color(self, color: tuple[float, float, float] = ..., *, composition: int | Proxy = ..., layer: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.layer.set-label-color  (operation, layer)
        Sets or clears the layer's Timeline label colour.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          layer  id  optional, defaults to the first selected layer (context layer)
              The layer to edit: a layer id, not its node's id. Defaults to the first selected layer.
              Unit: layer id (query:layers .id)
              Range: >= 1
          color  vec3  optional, default no label
              Label colour as (r, g, b) in 0..255. Omit to clear the label.
              Unit: label colour as [r, g, b], each an integer 0..255
              Range: 0..255

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.set-label-color","args":{}}]}}
          Python:
            bloom.ops.layer.set_label_color()
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.set-label-color","args":{}}]

        Related: query:layers, transact
        """
        ...
    def set_locked(self, locked: bool, *, composition: int | Proxy = ..., layer: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.layer.set-locked  (operation, layer)
        Locks or unlocks a layer. A locked layer and the nodes it depends on refuse edits.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          layer  id  optional, defaults to the first selected layer (context layer)
              The layer to edit: a layer id, not its node's id. Defaults to the first selected layer.
              Unit: layer id (query:layers .id)
              Range: >= 1
          locked  bool  required
              Whether the layer is locked.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.set-locked","args":{"locked":true}}]}}
          Python:
            bloom.ops.layer.set_locked(locked=True)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.set-locked","args":{"locked":true}}]

        Related: query:layers, transact
        """
        ...
    def set_mask_inverted(self, mask: int | Proxy, inverted: bool, *, composition: int | Proxy = ..., layer: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.layer.set-mask-inverted  (operation, layer)
        Inverts or restores a mask's coverage.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          layer  id  optional, defaults to the first selected layer (context layer)
              The layer to edit: a layer id, not its node's id. Defaults to the first selected layer.
              Unit: layer id (query:layers .id)
              Range: >= 1
          mask  id  required
              The layer mask to edit.
              Unit: mask id (query:layers .masks)
              Range: >= 1
          inverted  bool  required
              Whether the mask coverage is inverted.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.set-mask-inverted","args":{"mask":1,"inverted":true}}]}}
          Python:
            bloom.ops.layer.set_mask_inverted(mask=1, inverted=True)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.set-mask-inverted","args":{"mask":1,"inverted":true}}]

        Related: query:layers, transact
        """
        ...
    def set_mask_mode(self, mask: int | Proxy, mode: int, *, composition: int | Proxy = ..., layer: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.layer.set-mask-mode  (operation, layer)
        Sets how a mask combines with the masks above it.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          layer  id  optional, defaults to the first selected layer (context layer)
              The layer to edit: a layer id, not its node's id. Defaults to the first selected layer.
              Unit: layer id (query:layers .id)
              Range: >= 1
          mask  id  required
              The layer mask to edit.
              Unit: mask id (query:layers .masks)
              Range: >= 1
          mode  int  required
              0 None, 1 Add, 2 Subtract, 3 Intersect, 4 Difference.
              Range: 0..4
              Values:
                0  None
                1  Add
                2  Subtract
                3  Intersect
                4  Difference

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.set-mask-mode","args":{"mask":1,"mode":1}}]}}
          Python:
            bloom.ops.layer.set_mask_mode(mask=1, mode=1)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.set-mask-mode","args":{"mask":1,"mode":1}}]

        Related: query:layers, transact
        """
        ...
    def set_mask_path(self, mask: int | Proxy, path: Path, *, composition: int | Proxy = ..., layer: int | Proxy = ..., time: Time = ...) -> Mapping[str, Any] | None:
        """bloom.layer.set-mask-path  (operation, layer)
        Sets a mask's outline. On an animated mask path this keys the outline at `time`.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          layer  id  optional, defaults to the first selected layer (context layer)
              The layer to edit: a layer id, not its node's id. Defaults to the first selected layer.
              Unit: layer id (query:layers .id)
              Range: >= 1
          mask  id  required
              The layer mask to edit.
              Unit: mask id (query:layers .masks)
              Range: >= 1
          path  path  required
              The new outline in the layer's content space.
              Unit: {closed, anchors:[{point:[x, y], inHandle:[x, y]|null, outHandle:[x, y]|null,
              mode?}]} in the layer's content-space pixels; handles are relative to their anchor point
          time  time  optional, defaults to the playhead (frame 0 headless) (context time)
              An exact time. Defaults to the current playhead time.
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.set-mask-path","args":{"mask":1,"path":{"closed":true,"anchors":[{"point":[0,0],"inHandle":null,"outHandle":null},{"point":[100,0],"inHandle":null,"outHandle":null},{"point":[100,100],"inHandle":null,"outHandle":null},{"point":[0,100],"inHandle":null,"outHandle":null}]}}}]}}
          Python:
            bloom.ops.layer.set_mask_path(mask=1, path={"closed": True, "anchors": [{"point": (0, 0), "in_handle": None, "out_handle": None}, {"point": (100, 0), "in_handle": None, "out_handle": None}, {"point": (100, 100), "in_handle": None, "out_handle": None}, {"point": (0, 100), "in_handle": None, "out_handle": None}]})
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.set-mask-path","args":{"mask":1,"path":{"closed":true,"anchors":[{"point":[0,0],"inHandle":null,"outHandle":null},{"point":[100,0],"inHandle":null,"outHandle":null},{"point":[100,100],"inHandle":null,"outHandle":null},{"point":[0,100],"inHandle":null,"outHandle":null}]}}}]

        Related: bloom.layer.animate-mask-path, transact
        """
        ...
    def set_parent(self, parent: int | Proxy = ..., *, composition: int | Proxy = ..., layer: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.layer.set-parent  (operation, layer)
        Parents a layer to another layer's transform, or clears the parent.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          layer  id  optional, defaults to the first selected layer (context layer)
              The layer to edit: a layer id, not its node's id. Defaults to the first selected layer.
              Unit: layer id (query:layers .id)
              Range: >= 1
          parent  id  optional
              The parent layer. Omit to clear the parent.
              Unit: layer id (query:layers .id)
              Range: >= 1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.command-rejected
              When: The parent would form a cycle
              Do: choose a layer that is not a child of this one.
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.set-parent","args":{}}]}}
          Python:
            bloom.ops.layer.set_parent()
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.set-parent","args":{}}]

        Related: query:layers, transact
        """
        ...
    def set_range(self, start: Time, end: Time, *, composition: int | Proxy = ..., layer: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.layer.set-range  (operation, layer)
        Trims a layer: sets the composition span it is visible in, without moving its content. `end` is
        exclusive: the layer shows frames start..end-1.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          layer  id  optional, defaults to the first selected layer (context layer)
              The layer to edit: a layer id, not its node's id. Defaults to the first selected layer.
              Unit: layer id (query:layers .id)
              Range: >= 1
          start  time  required
              The layer's first frame.
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]
          end  time  required
              The layer's exclusive end, at most the composition duration.
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.command-rejected
              When: start is not before end, or end is past the composition duration
              Do: pass 0 <= start < end <= duration.
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.set-range","args":{"start":0,"end":24}}]}}
          Python:
            bloom.ops.layer.set_range(start=0, end=24)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.set-range","args":{"start":0,"end":24}}]

        Related: bloom.layer.set-timing, query:layers, transact
        """
        ...
    def set_solo(self, solo: bool, *, composition: int | Proxy = ..., layer: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.layer.set-solo  (operation, layer)
        Solos or unsolos a layer.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          layer  id  optional, defaults to the first selected layer (context layer)
              The layer to edit: a layer id, not its node's id. Defaults to the first selected layer.
              Unit: layer id (query:layers .id)
              Range: >= 1
          solo  bool  required
              Whether the layer is soloed.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.set-solo","args":{"solo":true}}]}}
          Python:
            bloom.ops.layer.set_solo(solo=True)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.set-solo","args":{"solo":true}}]

        Related: query:layers, transact
        """
        ...
    def set_time_modes(self, before: int, after: int, *, composition: int | Proxy = ..., layer: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.layer.set-time-modes  (operation, layer)
        Sets what a layer shows before and after its content: nothing, the first or last frame held, a
        loop or a ping-pong.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          layer  id  optional, defaults to the first selected layer (context layer)
              The layer to edit: a layer id, not its node's id. Defaults to the first selected layer.
              Unit: layer id (query:layers .id)
              Range: >= 1
          before  int  required
              Before the content: 0 None, 1 Hold, 2 Loop, 3 PingPong.
              Range: 0..3
              Values:
                0  None
                1  Hold
                2  Loop
                3  Ping-pong
          after  int  required
              After the content: 0 None, 1 Hold, 2 Loop, 3 PingPong.
              Range: 0..3
              Values:
                0  None
                1  Hold
                2  Loop
                3  Ping-pong

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.set-time-modes","args":{"before":0,"after":0}}]}}
          Python:
            bloom.ops.layer.set_time_modes(before=0, after=0)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.set-time-modes","args":{"before":0,"after":0}}]

        Related: bloom.layer.set-timing, query:layers, transact
        """
        ...
    def set_timing(self, startTime: Time, start: Time, end: Time, speedNumerator: int, speedDenominator: int, *, composition: int | Proxy = ..., layer: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.layer.set-timing  (operation, layer)
        Sets layer-owned time in one step (ADR 0024): where the layer's content starts (`startTime`,
        which moves content and keys), its visible span start..end (end exclusive), and an exact speed.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          layer  id  optional, defaults to the first selected layer (context layer)
              The layer to edit: a layer id, not its node's id. Defaults to the first selected layer.
              Unit: layer id (query:layers .id)
              Range: >= 1
          startTime  time  required
              Composition time of the layer's time zero (moves content and keys).
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]
          start  time  required
              The layer's first frame.
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]
          end  time  required
              The layer's exclusive end, at most the composition duration.
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]
          speedNumerator  int  required
              Exact speed numerator.
              Unit: exact speed ratio numerator / denominator, between 1/1000 and 1000
              Range: 1..2147483647
          speedDenominator  int  required
              Exact speed denominator.
              Unit: exact speed ratio numerator / denominator, between 1/1000 and 1000
              Range: 1..2147483647

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: The speed is outside 1/1000..1000
              Do: pick an exact ratio inside it.
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.set-timing","args":{"startTime":0,"start":0,"end":24,"speedNumerator":1,"speedDenominator":1}}]}}
          Python:
            bloom.ops.layer.set_timing(startTime=0, start=0, end=24, speedNumerator=1, speedDenominator=1)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.set-timing","args":{"startTime":0,"start":0,"end":24,"speedNumerator":1,"speedDenominator":1}}]

        Related: bloom.layer.set-range, bloom.layer.set-time-modes, query:layers, transact
        """
        ...
    def set_track_matte(self, source: int | Proxy, channel: int, inverted: bool, *, composition: int | Proxy = ..., layer: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.layer.set-track-matte  (operation, layer)
        Uses another layer of the same Merge as this layer's track matte.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          layer  id  optional, defaults to the first selected layer (context layer)
              The layer to edit: a layer id, not its node's id. Defaults to the first selected layer.
              Unit: layer id (query:layers .id)
              Range: >= 1
          source  id  required
              The matte source layer in the same Merge.
              Unit: layer id (query:layers .id)
              Range: >= 1
          channel  int  required
              0 Alpha, 1 Luma.
              Range: 0..1
              Values:
                0  Alpha
                1  Luma
          inverted  bool  required
              Whether the matte is inverted.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.set-track-matte","args":{"source":1,"channel":0,"inverted":false}}]}}
          Python:
            bloom.ops.layer.set_track_matte(source=1, channel=0, inverted=False)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.set-track-matte","args":{"source":1,"channel":0,"inverted":false}}]

        Related: bloom.layer.clear-track-matte, query:layers, transact
        """
        ...
    def split_at_time(self, *, composition: int | Proxy = ..., layer: int | Proxy = ..., time: Time = ...) -> Mapping[str, Any] | None:
        """bloom.layer.split-at-time  (operation, layer)
        Splits a layer at `time` into two layers (Ctrl+K): the original ends at `time` and a copy starts
        there.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          layer  id  optional, defaults to the first selected layer (context layer)
              The layer to edit: a layer id, not its node's id. Defaults to the first selected layer.
              Unit: layer id (query:layers .id)
              Range: >= 1
          time  time  optional, defaults to the playhead (frame 0 headless) (context time)
              An exact time. Defaults to the current playhead time.
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: layer (the new
          layer) (each {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.split-at-time","args":{}}]}}
          Python:
            bloom.ops.layer.split_at_time()
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.split-at-time","args":{}}]

        Related: query:layers, transact
        """
        ...
    def stop_animating_mask_path(self, mask: int | Proxy, path: Path, *, composition: int | Proxy = ..., layer: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.layer.stop-animating-mask-path  (operation, layer)
        Removes a mask path's keys and keeps one constant outline.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          layer  id  optional, defaults to the first selected layer (context layer)
              The layer to edit: a layer id, not its node's id. Defaults to the first selected layer.
              Unit: layer id (query:layers .id)
              Range: >= 1
          mask  id  required
              The layer mask to edit.
              Unit: mask id (query:layers .masks)
              Range: >= 1
          path  path  required
              The constant outline the mask keeps once its keys are removed.
              Unit: {closed, anchors:[{point:[x, y], inHandle:[x, y]|null, outHandle:[x, y]|null,
              mode?}]} in the layer's content-space pixels; handles are relative to their anchor point

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.layer.stop-animating-mask-path","args":{"mask":1,"path":{"closed":true,"anchors":[{"point":[0,0],"inHandle":null,"outHandle":null},{"point":[100,0],"inHandle":null,"outHandle":null},{"point":[100,100],"inHandle":null,"outHandle":null},{"point":[0,100],"inHandle":null,"outHandle":null}]}}}]}}
          Python:
            bloom.ops.layer.stop_animating_mask_path(mask=1, path={"closed": True, "anchors": [{"point": (0, 0), "in_handle": None, "out_handle": None}, {"point": (100, 0), "in_handle": None, "out_handle": None}, {"point": (100, 100), "in_handle": None, "out_handle": None}, {"point": (0, 100), "in_handle": None, "out_handle": None}]})
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.layer.stop-animating-mask-path","args":{"mask":1,"path":{"closed":true,"anchors":[{"point":[0,0],"inHandle":null,"outHandle":null},{"point":[100,0],"inHandle":null,"outHandle":null},{"point":[100,100],"inHandle":null,"outHandle":null},{"point":[0,100],"inHandle":null,"outHandle":null}]}}}]

        Related: bloom.layer.animate-mask-path, transact
        """
        ...

layer: LayerOps

class MergeOps:
    def reorder_input(self, merge: int | Proxy, slot: int | Proxy, index: int, *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.merge.reorder-input  (operation, merge)
        Moves one slot of a Merge node's layer stack to an index (0 is the topmost).

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          merge  id  required
              The Merge node.
              Unit: Merge node id (query:nodes)
              Range: >= 1
          slot  id  required
              The stack slot to move.
              Unit: layer-stack slot id (query:layers .slot)
              Range: >= 1
          index  int  required
              The slot's new position.
              Unit: zero-based position
              Range: >= 0

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.merge.reorder-input","args":{"merge":1,"slot":1,"index":0}}]}}
          Python:
            bloom.ops.merge.reorder_input(merge=1, slot=1, index=0)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.merge.reorder-input","args":{"merge":1,"slot":1,"index":0}}]

        Related: bloom.layer.move-before, query:nodes, transact
        """
        ...
    def set_enabled(self, merge: int | Proxy, enabled: bool, *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.merge.set-enabled  (operation, merge)
        Enables or bypasses a Merge node.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          merge  id  required
              The Merge node.
              Unit: Merge node id (query:nodes)
              Range: >= 1
          enabled  bool  required
              Whether the Merge is enabled.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.merge.set-enabled","args":{"merge":1,"enabled":true}}]}}
          Python:
            bloom.ops.merge.set_enabled(merge=1, enabled=True)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.merge.set-enabled","args":{"merge":1,"enabled":true}}]

        Related: query:nodes, transact
        """
        ...

merge: MergeOps

class NodeOps:
    def add(self, nodeType: str, position: tuple[float, float] = ..., *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.node.add  (operation, node)
        Adds a node of a registered type with its default parameters. query:node-types lists the types,
        their ports and parameters.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          nodeType  str  required
              The node type id (query node-types lists them).
              Unit: node type id (query:node-types)
          position  vec2  optional
              Graph layout position. Defaults to the origin.
              Unit: node-editor layout units

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: node (each
          {operationIndex, name, id}).

        Errors:
          bloom.scripting.command-rejected
              When: Unknown nodeType
              Do: pick an id from query {kind:"node-types"}.
          bloom.scripting.command-rejected
              When: The type allows one per composition (Merge, Output) and one exists
              Do: reuse the existing node.
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.node.add","args":{"nodeType":"bloom.value-scalar"}}]}}
          Python:
            bloom.ops.node.add(nodeType="bloom.value-scalar")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.node.add","args":{"nodeType":"bloom.value-scalar"}}]

        Related: query:node-types, bloom.node.connect, query:nodes, transact
        """
        ...
    def add_asset_source(self, asset: int | Proxy, position: tuple[float, float] = ..., *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.node.add-asset-source  (operation, node)
        Adds a source node for an asset, as dropping a footage asset on the Node canvas does: an Image
        Source for an image or image sequence, a Video Source for a video, an Audio Source for audio,
        whose asset parameter names the asset (image and video sources also take the asset's probed
        alpha association as their premultiply flag). The node is not wired: connect it with
        bloom.node.connect, or use bloom.layer.add-from-assets for a whole layer. Outputs: "node" (the
        new node id) and "parameter.<role>" for each of its parameters. Errors: "The asset does not
        exist"; a font or LUT asset is rejected (it makes no source). Undo: one step. MCP example:
        {"op":"bloom.node.add-asset-source","args":{"asset":3,"position":[0,0]}}.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              Id. The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          asset  id  required
              Id. An image, image sequence, video or audio asset.
              Unit: asset id (query:assets)
              Range: >= 1
          position  vec2  optional
              Vec2 [x, y] in Node canvas units (pixels at 100% zoom). Default [0, 0].
              Unit: node-editor layout units

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.node.add-asset-source","args":{"asset":1}}]}}
          Python:
            bloom.ops.node.add_asset_source(asset=1)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.node.add-asset-source","args":{"asset":1}}]

        Related: transact
        """
        ...
    def add_composition_source(self, source: int | Proxy, position: tuple[float, float] = ..., *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.node.add-composition-source  (operation, node)
        Adds a Composition Source node that renders another composition, as dropping a composition on
        the Node canvas does. Its composition parameter names `source`. Not wired;
        bloom.layer.add-from-assets makes a nested-composition layer instead. Outputs: "node" and
        "parameter.<role>" for each parameter. Errors: "Source composition does not exist"; a source
        that would nest the composition inside itself is rejected with the nesting cycle. Undo: one
        step. MCP example: {"op":"bloom.node.add-composition-source","args":{"source":2}}.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              Id. The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          source  id  required
              Id. The composition the node renders; not the target composition.
              Unit: layer id (query:layers .id)
              Range: >= 1
          position  vec2  optional
              Vec2 [x, y] in Node canvas units. Default [0, 0].
              Unit: node-editor layout units

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.node.add-composition-source","args":{"source":2}}]}}
          Python:
            bloom.ops.node.add_composition_source(source=2)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.node.add-composition-source","args":{"source":2}}]

        Related: transact
        """
        ...
    def add_fed_by(self, nodeType: str, node: int | Proxy, port: str, position: tuple[float, float] = ..., *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.node.add-fed-by  (operation, node)
        Adds a node fed by an existing output, in one step, as dragging a wire from an output socket to
        empty canvas and picking a node from the search does: `node`.`port` is connected to the new
        node's layer stack (a Merge) or its first input whose value kind matches. The node is placed to
        the right of `node`, in the nearest free spot. Outputs: "node", "editorNode" (both the new node
        id) and "parameter.<role>". Errors: "Node has no compatible socket" - no input of the new type
        matches the output's value kind; unknown node type or output; "No active composition". Undo: one
        step. MCP example:
        {"op":"bloom.node.add-fed-by","args":{"nodeType":"bloom.blur","node":5,"port":"image"}}.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              Id. The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          nodeType  str  required
              String. The node definition id of the new node.
              Unit: node type id (query:node-types)
          node  id  required
              Id. The node whose output feeds the new node.
              Unit: node id (query:nodes)
              Range: >= 1
          port  str  required
              String. That output's port name.
              Unit: port name (query:node-types lists each type's ports)
          position  vec2  optional
              Vec2 [x, y] in Node canvas units; the node is then moved beside `node`. Default [0, 0].
              Unit: node-editor layout units

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.node.add-fed-by","args":{"nodeType":"bloom.blur","node":1,"port":"image"}}]}}
          Python:
            bloom.ops.node.add_fed_by(nodeType="bloom.blur", node=1, port="image")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.node.add-fed-by","args":{"nodeType":"bloom.blur","node":1,"port":"image"}}]

        Related: transact
        """
        ...
    def add_feeding(self, nodeType: str, node: int | Proxy, port: str = ..., role: str = ..., position: tuple[float, float] = ..., *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.node.add-feeding  (operation, node)
        Adds a node that feeds an existing input, in one step, as dragging a wire backwards from an
        input socket to empty canvas and picking a node from the search does: the new node's first
        output whose value kind matches the input is connected to `node`.`port` (or to a new slot of a
        Merge stack when `role` is given), replacing what fed it. The node is placed to the left of
        `node`, in the nearest free spot. Outputs: "node", "editorNode" (both the new node id) and
        "parameter.<role>". Errors: "Node has no compatible socket" - no output of the new type matches
        the input's value kind; unknown node type or input; "No active composition". Undo: one step. MCP
        example:
        {"op":"bloom.node.add-feeding","args":{"nodeType":"bloom.value-scalar","node":5,"port":"radius"}}.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              Id. The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          nodeType  str  required
              String. The node definition id of the new node.
              Unit: node type id (query:node-types)
          node  id  required
              Id. The node whose input the new node feeds.
              Unit: node id (query:nodes)
              Range: >= 1
          port  str  optional
              String. That input's port name. Required unless role is given.
              Unit: port name (query:node-types lists each type's ports)
          role  str  optional
              String. A Merge stack role on `node` instead of a port; a new slot is made.
              Values:
                "content"  The slot's image (layer) input
                "audio"  The slot's audio input
          position  vec2  optional
              Vec2 [x, y] in Node canvas units; the node is then moved beside `node`. Default [0, 0].
              Unit: node-editor layout units

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.node.add-feeding","args":{"nodeType":"bloom.value-scalar","node":1,"port":"radius"}}]}}
          Python:
            bloom.ops.node.add_feeding(nodeType="bloom.value-scalar", node=1, port="radius")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.node.add-feeding","args":{"nodeType":"bloom.value-scalar","node":1,"port":"radius"}}]

        Related: transact
        """
        ...
    def arrange(self, nodes: tuple = ..., *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.node.arrange  (operation, node)
        Lays the node graph out from sources on the left to Composition Output on the right, as the Node
        editor's Organize > Arrange All (no `nodes`) or Arrange Selection (`nodes`) does. Arrange
        Selection lays out only those nodes and keeps their bounding box's top-left corner. Node groups
        and driver links are respected. Only layout positions change, never the graph or the render.
        Card sizes are the conservative document card size (the editor measures its cards, so its
        spacing can differ slightly). Outputs: none. Errors: "Node N is not in the composition" - a
        stale node id. An empty graph is a no-change. Undo: one step. MCP example:
        {"op":"bloom.node.arrange","args":{}}.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              Id. The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          nodes  array  optional
              Array of node ids to arrange among themselves. Default (omitted or empty): every node of
              the composition.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.node.arrange","args":{}}]}}
          Python:
            bloom.ops.node.arrange()
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.node.arrange","args":{}}]

        Related: transact
        """
        ...
    def connect(self, sourceNode: int | Proxy, sourcePort: str, targetNode: int | Proxy, port: str = ..., role: str = ..., slot: int | Proxy = ..., insertBefore: int | Proxy = ..., *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.node.connect  (operation, node)
        Connects an output port to an input port, or writes a Merge layer-stack slot when `role` is
        given. Kinds must match or promote (integer to scalar, boolean to integer or scalar, scalar to
        vector).

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          sourceNode  id  required
              The node that produces the value.
              Unit: node id (query:nodes)
              Range: >= 1
          sourcePort  str  required
              The output port name.
              Unit: port name (query:node-types lists each type's ports)
          targetNode  id  required
              The node, or Merge stack node, that consumes it.
              Unit: node id (query:nodes)
              Range: >= 1
          port  str  optional
              The input port name. Required unless a stack role is given.
              Unit: port name (query:node-types lists each type's ports)
          role  str  optional
              A Merge stack role. Giving it addresses the layer stack instead of a port.
              Values:
                "content"  The slot's image (layer) input
                "audio"  The slot's audio input
          slot  id  optional
              The stack slot to write. Omit it to create a new slot.
              Unit: layer-stack slot id (query:layers .slot)
              Range: >= 1
          insertBefore  id  optional
              The slot a new stack link lands above.
              Unit: layer-stack slot id (query:layers .slot)
              Range: >= 1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: edge;
          layerSlot when a new stack slot is created (each {operationIndex, name, id}).

        Errors:
          bloom.scripting.command-rejected
              When: The port kinds do not match
              Do: query:node-types shows each port's kind; insert a conversion node.
          bloom.scripting.command-rejected
              When: The link would form a cycle
              Do: connect a node that is not downstream.
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.node.connect","args":{"sourceNode":1,"sourcePort":"output","targetNode":1,"port":"input"}}]}}
          Python:
            bloom.ops.node.connect(sourceNode=1, sourcePort="output", targetNode=1, port="input")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.node.connect","args":{"sourceNode":1,"sourcePort":"output","targetNode":1,"port":"input"}}]

        Related: bloom.node.disconnect-input, query:nodes, query:node-types, transact
        """
        ...
    def disconnect_input(self, node: int | Proxy, port: str = ..., role: str = ..., slot: int | Proxy = ..., *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.node.disconnect-input  (operation, node)
        Clears one input port, or one Merge stack slot when `role` is given.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          node  id  required
              The node whose input is cleared.
              Unit: node id (query:nodes)
              Range: >= 1
          port  str  optional
              The input port name. Required unless a stack role is given.
              Unit: port name (query:node-types lists each type's ports)
          role  str  optional
              A Merge stack role.
              Values:
                "content"  The slot's image (layer) input
                "audio"  The slot's audio input
          slot  id  optional
              The stack slot to clear.
              Unit: layer-stack slot id (query:layers .slot)
              Range: >= 1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.node.disconnect-input","args":{"node":1,"port":"input"}}]}}
          Python:
            bloom.ops.node.disconnect_input(node=1, port="input")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.node.disconnect-input","args":{"node":1,"port":"input"}}]

        Related: bloom.node.connect, query:nodes, transact
        """
        ...
    def dissolve(self, node: int | Proxy, *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.node.dissolve  (operation, node)
        Removes a node and reconnects its upstream to its downstream where the kinds match.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          node  id  required
              The node to dissolve.
              Unit: node id (query:nodes)
              Range: >= 1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.node.dissolve","args":{"node":1}}]}}
          Python:
            bloom.ops.node.dissolve(node=1)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.node.dissolve","args":{"node":1}}]

        Related: bloom.node.remove, transact
        """
        ...
    def duplicate(self, offset: tuple[float, float] = ..., *, composition: int | Proxy = ..., nodes: tuple = ...) -> Mapping[str, Any] | None:
        """bloom.node.duplicate  (operation, node)
        Copies nodes with their parameters and the links between them.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          nodes  array  optional, defaults to the selected nodes (context selection)
              Node ids (Python also accepts proxies). Defaults to the current selection.
              Unit: array of node ids (query:nodes); defaults to the selected nodes
          offset  vec2  optional
              Layout offset for the copies. Defaults to (0, 0).
              Unit: node-editor layout units

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: node (one per
          copy) (each {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.node.duplicate","args":{}}]}}
          Python:
            bloom.ops.node.duplicate()
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.node.duplicate","args":{}}]

        Related: query:nodes, transact
        """
        ...
    def insert_reroute(self, node: int | Proxy, port: str = ..., role: str = ..., slot: int | Proxy = ..., position: tuple[float, float] = ..., *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.node.insert-reroute  (operation, node)
        Inserts a Reroute node into an existing link, as Shift+right-dragging across a wire does: the
        reroute is fed by the link's source and feeds the link's destination input, at `position`. Name
        the link by its destination input (`node` + `port`, or `role` + `slot` for a Merge stack input).
        Outputs: "node" and "editorNode" (the reroute's node id). Errors: "That link no longer exists" -
        nothing feeds that input; read the graph again. Undo: one step. MCP example:
        {"op":"bloom.node.insert-reroute","args":{"node":5,"port":"image","position":[200,0]}}.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              Id. The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          node  id  required
              Id. The node whose input the link feeds.
              Unit: node id (query:nodes)
              Range: >= 1
          port  str  optional
              String. That input's port name. Required unless role is given.
              Unit: port name (query:node-types lists each type's ports)
          role  str  optional
              String. A Merge stack role instead of a port; use with slot.
              Values:
                "content"  The slot's image (layer) input
                "audio"  The slot's audio input
          slot  id  optional
              Id. The Merge stack slot of that role.
              Unit: layer-stack slot id (query:layers .slot)
              Range: >= 1
          position  vec2  optional
              Vec2 [x, y] in Node canvas units. Default [0, 0].
              Unit: node-editor layout units

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.node.insert-reroute","args":{"node":1,"port":"image"}}]}}
          Python:
            bloom.ops.node.insert_reroute(node=1, port="image")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.node.insert-reroute","args":{"node":1,"port":"image"}}]

        Related: transact
        """
        ...
    def move(self, positions: tuple, groups: tuple = ..., *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.node.move  (operation, node)
        Moves node cards in the node editor and optionally changes their group membership.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          positions  array  required
              Layout positions as (node, x, y).
          groups  array  optional
              Group membership as (node, group); a group of 0 leaves the node ungrouped.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.node.move","args":{"positions":[[1,0,0]]}}]}}
          Python:
            bloom.ops.node.move(positions=((1, 0, 0),))
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.node.move","args":{"positions":[[1,0,0]]}}]

        Related: query:nodes, transact
        """
        ...
    def remove(self, *, composition: int | Proxy = ..., nodes: tuple = ...) -> Mapping[str, Any] | None:
        """bloom.node.remove  (operation, node)
        Removes nodes. Removing a layer's Layer Output node deletes the layer (query:layers reports it
        as layerOutputNode).

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          nodes  array  optional, defaults to the selected nodes (context selection)
              Node ids (Python also accepts proxies). Defaults to the current selection.
              Unit: array of node ids (query:nodes); defaults to the selected nodes

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.node.remove","args":{}}]}}
          Python:
            bloom.ops.node.remove()
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.node.remove","args":{}}]

        Related: bloom.node.dissolve, query:layers, transact
        """
        ...
    def set_collapsed(self, node: int | Proxy, collapsed: bool, *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.node.set-collapsed  (operation, node)
        Collapses or expands a node card.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          node  id  required
              The node to edit.
              Unit: node id (query:nodes)
              Range: >= 1
          collapsed  bool  required
              Whether the node is collapsed.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.node.set-collapsed","args":{"node":1,"collapsed":true}}]}}
          Python:
            bloom.ops.node.set_collapsed(node=1, collapsed=True)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.node.set-collapsed","args":{"node":1,"collapsed":true}}]

        Related: transact
        """
        ...
    def set_muted(self, node: int | Proxy, muted: bool, *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.node.set-muted  (operation, node)
        Mutes or unmutes a node: a muted node passes its main input through.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          node  id  required
              The node to edit.
              Unit: node id (query:nodes)
              Range: >= 1
          muted  bool  required
              Whether the node is muted.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.node.set-muted","args":{"node":1,"muted":true}}]}}
          Python:
            bloom.ops.node.set_muted(node=1, muted=True)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.node.set-muted","args":{"node":1,"muted":true}}]

        Related: transact
        """
        ...
    def set_parameter_value(self, node: int | Proxy, role: str, integer: int = ..., number: float = ..., boolean: bool = ..., text: str = ..., vector: Value = ..., *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.node.set-parameter-value  (operation, node)
        Sets one node parameter to a constant by its role, as the Properties panel's typed controls do:
        dropdowns (an enum index, as `integer`), switches (`boolean`), text fields (`text`), numbers
        (`number`) and vectors or colours (`vector`, 2 to 4 numbers). Give one value argument; if
        several are given the first of integer, number, boolean, text, vector is used. A role the node
        type declares as optional and the node does not hold yet is created, as the panel does. An
        animated or driven parameter becomes a constant. Outputs: "parameter" when an optional parameter
        was created. Errors: "Node does not exist"; "The node has no such optional parameter" - a role
        the node type does not declare (query kind=node-types lists the roles); "The value violates the
        parameter schema" - the wrong value kind or a value out of range. Undo: one step. MCP example:
        {"op":"bloom.node.set-parameter-value","args":{"node":5,"role":"blendMode","integer":3}}.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              Id. The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          node  id  required
              Id. The node.
              Unit: node id (query:nodes)
              Range: >= 1
          role  str  required
              String. The parameter's role on the node type (for example "blendMode", "text",
              "enabled").
          integer  int  optional
              Integer. A whole number or an enum index (dropdowns).
          number  float  optional
              Number. A real value.
          boolean  bool  optional
              Boolean. A switch.
          text  str  optional
              String. A text value.
          vector  value  optional
              Value. A vec2 [x, y], vec3 [x, y, z] or colour [r, g, b, a] (scene-linear, alpha 0..1).
              Unit: the parameter's own width: a number, [x, y], [x, y, z] or [r, g, b, a]

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.node.set-parameter-value","args":{"node":1,"role":"blendMode","integer":0}}]}}
          Python:
            bloom.ops.node.set_parameter_value(node=1, role="blendMode", integer=0)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.node.set-parameter-value","args":{"node":1,"role":"blendMode","integer":0}}]

        Related: transact
        """
        ...
    def set_video_audio_stream(self, node: int | Proxy, stream: int, *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.node.set-video-audio-stream  (operation, node)
        Chooses which audio stream of a Video Source node's file plays, as the Audio Stream dropdown in
        the Properties panel's Video section does. `stream` is the container's stream index as the
        asset's probe lists it. Outputs: none. Errors: the node is not a Video Source, or its asset has
        no audio stream with that index. Undo: one step. MCP example:
        {"op":"bloom.node.set-video-audio-stream","args":{"node":5,"stream":1}}.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              Id. The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          node  id  required
              Id. The Video Source node.
              Unit: node id (query:nodes)
              Range: >= 1
          stream  int  required
              Integer >= 0. The container's audio stream index.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.node.set-video-audio-stream","args":{"node":1,"stream":1}}]}}
          Python:
            bloom.ops.node.set_video_audio_stream(node=1, stream=1)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.node.set-video-audio-stream","args":{"node":1,"stream":1}}]

        Related: transact
        """
        ...
    def set_width(self, node: int | Proxy, width: float, *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.node.set-width  (operation, node)
        Sets a node card's width.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          node  id  required
              The node to edit.
              Unit: node id (query:nodes)
              Range: >= 1
          width  float  required
              The node's layout width.
              Unit: node-editor layout units

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.node.set-width","args":{"node":1,"width":160}}]}}
          Python:
            bloom.ops.node.set_width(node=1, width=160)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.node.set-width","args":{"node":1,"width":160}}]

        Related: transact
        """
        ...

node: NodeOps

class NodeGroupOps:
    def create(self, name: str = ..., *, composition: int | Proxy = ..., nodes: tuple = ...) -> Mapping[str, Any] | None:
        """bloom.node-group.create  (operation, node-group)
        Frames nodes in a named group. Groups are layout only and never change evaluation.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          nodes  array  optional, defaults to the selected nodes (context selection)
              Node ids (Python also accepts proxies). Defaults to the current selection.
              Unit: array of node ids (query:nodes); defaults to the selected nodes
          name  str  optional
              The frame's name. Defaults to "Group".

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. outputs names: nodeGroup
          (each {operationIndex, name, id}).

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.node-group.create","args":{}}]}}
          Python:
            bloom.ops.node_group.create()
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.node-group.create","args":{}}]

        Related: query:nodes, transact
        """
        ...
    def remove(self, group: int | Proxy, *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.node-group.remove  (operation, node-group)
        Removes a group frame; its nodes stay.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          group  id  required
              The group to remove.
              Unit: node group id (query:nodes .group)
              Range: >= 1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.node-group.remove","args":{"group":1}}]}}
          Python:
            bloom.ops.node_group.remove(group=1)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.node-group.remove","args":{"group":1}}]

        Related: query:nodes, transact
        """
        ...
    def rename(self, group: int | Proxy, name: str, *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.node-group.rename  (operation, node-group)
        Renames a group frame.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          group  id  required
              The group to rename.
              Unit: node group id (query:nodes .group)
              Range: >= 1
          name  str  required
              The new name.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.node-group.rename","args":{"group":1,"name":"Group"}}]}}
          Python:
            bloom.ops.node_group.rename(group=1, name="Group")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.node-group.rename","args":{"group":1,"name":"Group"}}]

        Related: query:nodes, transact
        """
        ...
    def set_members(self, group: int | Proxy, *, composition: int | Proxy = ..., members: tuple = ...) -> Mapping[str, Any] | None:
        """bloom.node-group.set-members  (operation, node-group)
        Sets the nodes a group frames.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          group  id  required
              The group to edit.
              Unit: node group id (query:nodes .group)
              Range: >= 1
          members  array  optional, defaults to the selected nodes (context selection)
              Node ids (Python also accepts proxies). Defaults to the current selection.
              Unit: array of node ids (query:nodes); defaults to the selected nodes

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.node-group.set-members","args":{"group":1}}]}}
          Python:
            bloom.ops.node_group.set_members(group=1)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.node-group.set-members","args":{"group":1}}]

        Related: query:nodes, transact
        """
        ...

node_group: NodeGroupOps

class ParameterOps:
    def reset(self, parameter: int | Proxy, *, composition: int | Proxy = ..., time: Time = ...) -> Mapping[str, Any] | None:
        """bloom.parameter.reset  (operation, parameter)
        Resets a node parameter to its node type's default, as "Reset to default" on a Properties row
        does. A driven parameter is disconnected (restoring the default); a constant parameter takes the
        default value; an animated parameter gets a key with the default value at `time` and keeps its
        other keys (convert it to constant first, with bloom.animation.convert-to-constant, to drop the
        animation). Outputs: none, or the key's "keyframe" output for an animated parameter. Errors:
        "The parameter is not in the composition" - a stale id; "The parameter has no node default to
        reset to" - it does not belong to a node (a layer-stack or composition value); a text, enum or
        path default cannot be keyed on an animated parameter. Undo: one step. MCP example:
        {"op":"bloom.parameter.reset","args":{"parameter":12}}.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              Id. The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          time  time  optional, defaults to the playhead (frame 0 headless) (context time)
              Time. For an animated parameter only: the key time of the default key, a whole frame or an
              exact [numerator, denominator] in seconds, as
              bloom.animation.set-keyframe-at-time-for-parameter takes it. Defaults to the playhead.
              Unit: frames at the composition's frame rate (an integer), or exact seconds as [numerator,
              denominator]
          parameter  id  required
              Id. The parameter (query kind=parameters lists ids).
              Unit: parameter id (query:layers .parameters, query:nodes .parameters)
              Range: >= 1

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.parameter.reset","args":{"parameter":1}}]}}
          Python:
            bloom.ops.parameter.reset(parameter=1)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.parameter.reset","args":{"parameter":1}}]

        Related: transact
        """
        ...
    def set_source(self, parameter: int | Proxy, value: Value = ..., curve: int | Proxy = ..., driverNode: int | Proxy = ..., driverPort: str = ..., *, composition: int | Proxy = ...) -> Mapping[str, Any] | None:
        """bloom.parameter.set-source  (operation, parameter)
        Sets what drives a parameter: exactly one of a constant `value`, an existing `curve`, or a
        value-graph `driverNode` with `driverPort`. Enumerations (blend mode, loop mode, text alignment
        and so on) take either the stored value or its label, matched case-insensitively (3 or "screen"
        for the Screen blend mode); query {kind:"parameter"} lists the choices.

        Arguments:
          composition  id  optional, defaults to the current composition (the Viewer's in the desktop application, the first one headless) (context composition)
              The composition to edit. Defaults to the current composition.
              Unit: composition id (query:compositions)
              Range: >= 1
          parameter  id  required
              The parameter to repoint.
              Unit: parameter id (query:layers .parameters, query:nodes .parameters)
              Range: >= 1
          value  value  optional
              A constant value. Give exactly one of value, curve or driverNode.
              Unit: the parameter's own width: a number, [x, y], [x, y, z] or [r, g, b, a]
          curve  id  optional
              An existing animation curve, instead of value.
              Unit: animation curve id (query:keyframes, or the parameter's curveId)
              Range: >= 1
          driverNode  id  optional
              A value-graph node that drives it, instead of value (with driverPort).
              Unit: node id (query:nodes)
              Range: >= 1
          driverPort  str  optional
              The driving node's output port.
              Unit: port name (query:node-types lists each type's ports)

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.
          - Existing ids for the id arguments; each argument's unit names the query that lists them.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: None of value, curve or driverNode is given
              Do: pass exactly one.
          bloom.scripting.command-rejected
              When: An unknown label or an out-of-range value of an enumeration
              Do: the message names the valid choices.
          bloom.scripting.command-rejected
              When: The value does not fit the parameter's kind or range
              Do: query {kind:"parameter"} reports valueKind, range and choices.
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.parameter.set-source","args":{"parameter":1,"value":1}}]}}
          Python:
            bloom.ops.parameter.set_source(parameter=1, value=1)
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.parameter.set-source","args":{"parameter":1,"value":1}}]

        Related: query:parameter, bloom.animation.set-keyframe-at-time-for-parameter, transact
        """
        ...

parameter: ParameterOps

class ProjectOps:
    def set_name(self, name: str) -> Mapping[str, Any] | None:
        """bloom.project.set-name  (operation, project)
        Renames the project.

        Arguments:
          name  str  required
              The new project name.

        Effects: undoable: one undo step per transact call; marks the project unsaved.

        Prerequisites:
          - The current revision, from any query result, as transact's expectedRevision.

        Result:
          The transact result {succeeded, revision, diagnostics, outputs}. This operation creates no
          ids, so outputs is empty.

        Errors:
          bloom.scripting.invalid-argument
              When: an argument is missing, of the wrong kind or out of range
              Do: the message names the argument and a working example call
          bloom.scripting.command-rejected
              When: the document refused the edit (a stale id, a locked layer, an impossible value)
              Do: re-read the ids with the related query and fix the value
          bloom.scripting.stale-revision  (retry)
              When: the project moved on since expectedRevision
              Do: re-query and retry with the current revision

        Examples:
          MCP:
            {"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":"bloom.project.set-name","args":{"name":"Title study"}}]}}
          Python:
            bloom.ops.project.set_name(name="Title study")
          CLI:
            bloom-cli run edits.json   # [{"op":"bloom.project.set-name","args":{"name":"Title study"}}]

        Related: query:project, transact
        """
        ...

project: ProjectOps

def registry() -> Mapping[str, Operation]: ...
def get(operation_id: str) -> Operation: ...
