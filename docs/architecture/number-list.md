# Number List

Status: built (#627). This document owns the number list: the value, its socket, what connects to
it, how it is authored, saved, read and cached. It is Bloom's only list kind. The code that owns
each part names this document. Nodes that work on a list as a whole (#628) and conversions from a
list to a path, an image or points (#629) are built on this value and are not part of it.

## The value

`document::NumberListValue` is an ordered list of finite numbers (`double`). Order is authored and
significant. The empty list is a value.

| Limit | Value |
| --- | --- |
| Items in one list (`kMaximumNumberListItems`) | 65536 |

A list is general: it carries no unit, no range and no meaning of its own. What a list's numbers
are (spectrum bands, samples, weights) is decided by the node that produces it.

A list parameter is a constant or is driven by another node's output. No curve kind carries a
list, so a list cannot be keyed: creating an animation for a list parameter is refused.

## The socket

`SocketValueKind::NumberList` and `ParameterValueKind::NumberList` are both named `number-list`.
The socket is drawn as an ordinary circular value socket in pale mint (`SocketNumberList`,
`docs/ux/visual-language.md`) and is labelled "Number List".

### What connects

A list input takes a list, and every value that is one or more numbers in a fixed order:

| From | To | Meaning |
| --- | --- | --- |
| `Scalar`, `Integer`, `Boolean` | `NumberList` | A list of that one number (`false` is 0, `true` is 1) |
| `Vector2`, `Vector3` | `NumberList` | The components in order: x, y (, z) |
| `Color` | `NumberList` | Red, green, blue, alpha |

Each is its own compiled promotion (`ValuePromotion::ScalarToList` and its siblings), visible in a
plan like every other promotion.

Nothing promotes out of a list. Reading one item, or folding a list to one number, is a choice a
node makes (which item? which fold?), never a connection. A `String`, a `Path`, a field, an image
and audio do not become a list.

A refused connection says which rule it broke:

- a list output into any other input: "... cannot connect to the ...: a number list connects only
  to a Number List input";
- anything else into a list input: "... : a Number List input takes a number list, a number, a
  vector or a color".

`document::isAcceptedSocketConnection()` is the one predicate, as for every other kind.

No built-in node has a list input until the list nodes of #628 and #629 exist; the promotions are
part of the kind so those nodes need no rule of their own.

## The Number List node

**Number List** (`bloom.value-number-list`, in Values) holds an authored list on its `value`
output. Like every value constant it has no input.

On the card and in Properties the list is one text field: its numbers separated by commas or
spaces, in the C locale. The field shows the shortest text that reads back as the same numbers.
Ending the edit (Return, Escape, Tab or focus loss) commits one transaction; text that is not a
list is refused with the reason ("\"x\" is not a number.", "A list holds at most 65536 numbers.")
and nothing is written. An empty field is the empty list.

A node's list output is read out in Properties like any other value output: its length and its
first four numbers ("12 items: 0.5, 0.25, 0, 1, …").

## Reading a list

No image node reads a list, so a list output cannot be read through a private sink the way a
Scalar or a Path is. `SnapshotCompileRequest::valueReadouts` names value nodes to compile although
nothing in the picture reads them, as a Layer Bounds readout is. Properties uses it for a selected
node's list outputs, and `scripting::queryNodeNumberList()` uses it to answer one list output at
one time.

**Write to a Number List** (`bloom.node.write-to-number-list`, from MCP, Python
`bloom.ops.node.write_to_number_list` and a `bloom-cli run` script) stores what a node's list
output gives at a time in a Number List node: a new one beside the source, wired to nothing, or a
`target` Number List node whose constant is replaced (a driven target is refused). The node is
evaluated off the UI thread when the operation is created and stops when the request is
cancelled. It is one undo step.

## Scripting

A list argument and a list read back have one shape on every route:

- MCP and `bloom-cli run`: `{"values": [1, 2.5, -3]}`.
- Python: `{'values': [1, 2.5, -3]}`; a value read from `bloom.data` is a frozen mapping holding
  a tuple, and `dict(value)` is an argument as it stands.

The object holds exactly `values`. An item that is not a finite number, a boolean item, a nested
list or more than 65536 items is refused before anything reaches the document.

## The project file

A list constant is `{"kind": "numberList", "values": [n, ...]}` (the `numberListValue-1.44` arm
of `parameterValue-1.4`, `project-format.md`). Items are written as every other Float64 is. A
build that predates the kind does not know the discriminator and opens such a project as
preserved read-only, under the format's rule for unknown core discriminators; projects written
before the kind hold no list and need no migration.

## Evaluation and cache identity

A compiled list is `runtime::CompiledNumberList`, a shared immutable vector, one of the
alternatives of `CompiledValue`. A list operand enters an operation's cache key as its item count
followed by every item's bits, so the same list is the same cached result and a changed item or a
changed length is a different one.

One evaluation's list outputs may hold `ValueGraphMemoization::maxRetainedListItems` items
together (sixteen whole lists by default). An output that is too long, or that would take the
outputs past that allowance, reads empty with the diagnostic "Value graph list budget exceeded";
the failure is not cached as a result.

## Tests

- `bloom.runtime.value_graph`: list literals, the six promotions, cache identity, the item budget.
- `bloom.runtime.number-list`: validity, connection rules and refusals, authoring and undo, that
  a list cannot be keyed, the read of an unconsumed list, Write to a Number List.
- `bloom.project.value-graph`: save and reopen byte identical, decode refusals, the length limit.
- `bloom.scripting.python-api` and `bloom.scripting.mcp-pipes`: authoring, reading back, Write to a
  Number List and refusals from Python, `bloom-cli` and MCP.
- `bloom.ui.node-field-controls`: the list editor on the card and in Properties.
