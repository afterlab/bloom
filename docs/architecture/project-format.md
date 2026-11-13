# Bloom Project Format

The project format has one current schema: container `1.0`, document schema `1.0`
(`schemas/project/document-1.0.schema.json`) and manifest schema `1.0`
(`schemas/project/manifest-1.0.schema.json`). There is no older version and nothing converts a
file from one: a document either is `1.0`, is a newer minor of major 1 opened through the
preservation rules below, or is refused.

Status: accepted

Implementation status: bounded reservations and PMR allocation, canonical integer/rational,
Base64, UTF-8 string, and shared count/write JSON-layout primitives, canonical manifest encoding
and schema checks, the normative document `1.0` schema artifact and checks, manifest requirement
validation, durable allocator high-water state, opaque extension envelopes, the Linux
staged-artifact close/reopen verification foundation, the canonical document writer over immutable
snapshots with explicitly supplied color settings, strict bounded JSON parsing into a Bloom-owned
DOM, typed document decode and reconstruction through checked model surfaces, newer-minor
unknown-member round-trip capture and write overlay, and the constrained ZIP container reader and
writer are implemented. Format-specific semantic verification of the complete save/reopen
pipeline, and cross-platform publication parity remain pending.

Updated: 2026-11-12

## Purpose And Ownership

`.bloom` is Bloom's durable authoring container. It preserves canonical project truth needed to
reopen and evaluate a project; it is not a general VFX interchange format. Adopted standards remain
explicit adapters at their own boundaries.

`src/project` owns schemas, constrained-container validation, deterministic encoding,
unknown-member round-trip state, and save staging. `src/platform` owns narrow filesystem identity,
flush, and atomic-publication primitives. `src/document` owns format-independent durable values and
validation. No JSON or ZIP implementation type enters document, command, runtime, UI, or public
extension contracts.

The application-level Open/Save lifecycle, supersession, dirty state, and unsaved-change flow are
defined in [`project-session.md`](project-session.md).

[ADR 0015](../decisions/0015-zip-json-project-container.md) fixes the container choice; this document
is its normative v1 implementation contract.

## Version 1 Constants

The container version is `1.0` and the document schema version is `1.0`. Version objects always
contain JSON-number members in `major`, `minor` order. Each is an unsigned 32-bit integer.

The schemas use JSON Schema Draft 2020-12, declare the 2020-12 `$schema`, and use only
repository-local `$ref` targets. Their `$id`s are `urn:afterlab:bloom:schema:project-document:1.0`
and `urn:afterlab:bloom:schema:project-manifest:1.0`. Some `$defs` names carry a numeric suffix
(`layerOutput-1.37`, `parameterSource-1.4`); the suffix is part of the definition's name only and
names no document version. The manifest artifact requires container `1.0` and declares document
schema `1.0` through the same `documentSchemaVersion` definition the document uses.

The repository check `bloom.quality.project_schemas` keeps exactly this pair in
`schemas/project`. It checks the header, the version, the definitions Project I/O relies on (decimal
patterns, text ceilings, rationals, color settings, the manifest generator), the rules every schema
keeps (references resolve, every definition is reachable, every described object says whether it
keeps unknown members, every array is bounded), and finally every schema node against the lock
`tools/quality/project_schema_checks/project_schema_lock.cpp`. A schema edit regenerates the lock
with `generate_schema_lock` in the same change. A generic schema validator is useful for
fixtures, but it does not replace Bloom's duplicate-key, resource, canonical-decimal,
cross-reference, allocator, graph, and preservation validation.

The project definition requires `colorSettings`; the OCIO reference definition requires every
member specified below and selects one closed locator shape with `oneOf`. These definitions
validate structure and lexical bounds. Project I/O additionally validates locator normalization,
digest spelling, sorted context variables, locator/portability agreement, and the fixed v1 process
Color Interop ID. The manifest's `requirement-1.0` definition carries the exact provider,
capability, schema-version, and node-type-coverage members specified below.

Known object definitions permit additional members through `unevaluatedProperties: true` so a
compatible newer minor can be retained. Discriminator values and array element categories remain
closed. The compatibility decoder, not the generic schema alone, determines whether an unknown
member is safely additive.

Asset kinds are `image`, `sequence`, `audio`, `video` and `lut`; the schema also names the retired
`font` kind, which the writer never emits. LUT records use the file locator,
content digest, name, folder, tags and order; dimensions are zero and there is no sequence or
video-stream descriptor. LUT bytes stay external assets. The opaque data-block projection identifies
them as `bloom.color.lut-asset` without introducing another data-block kind or duplicating the
durable asset record.

`portability: "external"` serves file assets whose path cannot be represented as a project-relative
path, including different Windows drives and colon-bearing mounted path components. `path` and
`relinkHint` both contain the same absolute RFC 8089 file URI. A project-relative path keeps its
relative-first resolution and absolute fallback hint. Saving never rewrites a locator solely because
the project moved. Relink can replace either form with a fresh locator.

Both OCIO nodes use registry schema version 1 and ordinary node/parameter persistence. Their
constant parameters are `from`, `to`, `bypass` for CST, and `lut` (integer asset id),
`interpolation` (0 Linear, 1 Tetrahedral, 2 Best), `direction` (0 Forward, 1 Inverse),
`processSpace`, `bypass`, `look` for File Transform. Empty space ids mean the composition working
space. `bypassLookNodes` is a request option and is never stored in the project.

Every asset interpretation carries `inputColorSpaceId`, and image and video source nodes carry the
matching non-animatable String parameter. The empty id is Auto/inherit; a non-empty id is an exact
non-data color-space id in the project's OCIO configuration. The integer interpretation and
image/video `colorSpace` parameter remain (see "Asset Input Color Spaces").

The text source stores a font ID and optional box, wrap, vertical-alignment, anchor, and overflow
parameters (see "Font IDs And Text Layout"). Fonts are not assets ([Fonts](fonts.md)): a text node
stores a font ID, and no font file is bundled or saved.

The `vec3` constant value is the third authoring vector width, with its own kind token rather than a
third component on `vec2`, because the discriminators name TYPES. The `driver` parameter source
carries `sourceNodeId` and `outputPort`, structurally an edge source that lands in parameter-address
space; the writer can describe every parameter source the document layer holds.

`manifest.json` and `document.json` are strict UTF-8 [RFC 8259] JSON. The reader rejects a BOM,
invalid UTF-8 or Unicode scalar sequences, lone escaped surrogates, duplicate decoded object keys,
comments, trailing commas, non-finite values, and JSON5 extensions before constructing trusted
document state.

### Path values

A constant may be a `path` value:

```json
{"kind":"path","anchors":[{"point":{"x":0,"y":0},"outHandle":{"x":20,"y":0}},{"point":{"x":40,"y":40},"inHandle":{"x":40,"y":20}}],"closed":false}
```

Canonical member order is `kind`, `anchors`, `closed`. Each anchor has `point`, then optional
`inHandle` and `outHandle`, in that order. Points and handles have exactly `x`, `y`, both finite
Float64 coordinates in full-resolution author pixels. Handles are absolute positions; an omitted
handle coincides with its anchor. Anchor and point objects are closed, including for newer minors.
The path value envelope retains the usual additive-member preservation contract.

There are at most 4,096 anchors, checked before decoded anchor allocation; empty paths and open
paths are valid. `PathValue` is a separate document parameter alternative with no numeric
promotion; it animates as a whole value (see "Layer Masks") and may hold several contours (see
"Compound Paths"). The schema's `-1.14` `$defs` hold this structural contract; typed validation
also enforces finite coordinates.

## Constrained ZIP Profile

Container `1.0` is aligned with the core profile in [ISO/IEC 21320-1] and contains exactly two
regular-file entries in this order:

```text
manifest.json
document.json
```

Both names are exact ASCII and carry the UTF-8 flag. Container `1.0` rejects every other entry,
including a manifest-declared payload. A later minor version may add payload entries only after
Project I/O can preserve their bounded bytes and metadata while their owner is unavailable.

The accepted reader profile is deliberately smaller than general ZIP:

- single-disk archive with no encryption, archive comment, entry comment, digital signature,
  prepended data, trailing data, or nested archive interpretation
- stored method `0` or deflate method `8`; no ZIP64 because all v1 limits are below classic ZIP
  limits
- no data descriptors or unapproved extra fields; sizes and CRC are present in local and central
  headers
- local and central names, flags, method, sizes, CRC, and entry counts agree
- entry byte ranges do not overlap each other or the central directory
- entries are regular non-executable files; directories, symlinks, hard-link metadata, devices,
  FIFOs, sockets, and platform-special file types are rejected
- no absolute name, backslash, NUL, empty component, `.`, or `..`; the exact two-name allowlist is
  still checked after path-profile validation
- each entry is streamed into a bounded buffer and its complete CRC is verified before JSON parsing
- normal Open never extracts an archive to a directory

The writer emits no comments, descriptors, extra fields, encryption, ZIP64, or platform-dependent
paths. It writes manifest then document, uses the DOS epoch `1980-01-01 00:00:00`, declares regular
mode `0644` without executable bits, and uses deflate level 6. If deflate would not reduce an entry,
the writer stores that entry instead. Older Bloom readers that enforce the former `1000:1` expansion
ratio may reject newly written, highly compressible projects and need an update. Existing stored and
deflated projects remain readable by the current reader. Exact whole
archive bytes are not a compatibility promise because a qualified compression implementation may
change; decoded semantics and the uncompressed JSON bytes are deterministic.

### Resource Limits

The v1 writer refuses to create a project outside these limits, so every conforming v1 file written
by Bloom is accepted by the default reader. Limits apply before allocation where the size is known
and during streaming otherwise.

| Resource | v1 limit |
| --- | ---: |
| physical archive size | 260 MiB (`272629760` bytes) |
| entry count | exactly `2` |
| expanded `manifest.json` | 1 MiB (`1048576` bytes) |
| expanded `document.json` | 256 MiB (`268435456` bytes) |
| total expanded bytes | 257 MiB (`269484032` bytes) |
| JSON nesting depth, root included | `128` |
| JSON values across both entries | `4000000` |
| elements in one array or members in one object | `1000000` |
| one decoded ordinary JSON string, excluding extension `payload` | 64 MiB (`67108864` UTF-8 bytes) |
| one extension `payload` base64 spelling | 89478488 ASCII bytes |
| one decoded opaque extension payload | 64 MiB (`67108864` bytes) |
| all extension `payload` base64 spellings | 192 MiB (`201326592` ASCII bytes) |
| all decoded opaque extension payloads | 128 MiB (`134217728` bytes) |
| default aggregate resident Project I/O memory per operation | 1 GiB (`1073741824` bytes) |
| application-wide resident Project I/O memory across concurrent operations | 2 GiB (`2147483648` bytes) |
| one Project I/O allocation | 256 MiB (`268435456` bytes) |

Namespaced owner, type, provider, capability, and remapper IDs are ASCII lowercase identifiers
matching `[a-z0-9][a-z0-9._-]{0,127}`. Structural schema keys, port names, roles, media types, and
reference-table keys are at most 256 UTF-8 bytes and non-empty. Human-facing project, composition,
and layer names are at most 4096 UTF-8 bytes and non-empty. More restrictive owning schemas may set
smaller bounds.

Open options may impose lower deployment or test budgets. Raising any limit requires an explicit
headless/application policy bounded by the implementation's checked absolute ceiling; it never
happens implicitly because a file requests more memory.

The resident budget is not an archive-size alias. It charges all simultaneously live memory owned
or conservatively reserved by one Open, Save, or Save Copy operation: compressed input,
expanded entries, parser DOM and indexes, duplicate-key tables, decoded `Document`,
`RoundTripState`, opaque payloads, preserved archive bytes, canonical output, and reopen-validation
state. The operation allocator performs checked addition before every reservation and rejects an
individual allocation above 256 MiB. The application-owned `ProjectIoMemoryCoordinator` also
charges the same reservation against one 2 GiB pool shared by every concurrent Project I/O task;
headless entry points construct the same bounded coordinator. Third-party work must use the
operation allocator or reserve a worst-case charge before calling a dependency; unmetered
dependency allocation is not permitted. Moving storage between stages transfers its charge, while
keeping two representations live charges both.

`OpenOptions` and `SaveOptions` default the per-operation resident budget to 1 GiB and may lower it.
No operation budget may exceed the shared 2 GiB implementation ceiling. Per-operation and shared
charges are acquired atomically or not at all, and are released when their owning allocation or
reservation dies. A size conversion, multiplication, accumulation, or allocator charge that
overflows is resource exhaustion, never a wrapped smaller request. Allocation failure and budget
rejection produce typed `ProjectIoCode::ResourceExhausted`, release every Project I/O allocation,
close handles, and invoke RAII cleanup for any unpublished stage. Open returns without constructing
or installing partial trusted state; Save returns `FailedBeforePublication` and never enters the
publication lease. Cleanup failure remains a secondary diagnostic and does not hide the
resource-exhaustion cause.

The base64 spelling bound is exactly `4 * ceil(67108864 / 3)` and is measured on the ASCII value
after JSON string unescaping. It is a narrow exception to the ordinary decoded-string bound, not
permission for another giant string. The aggregate spelling, decoded-payload, expanded-entry, and
total-expanded limits all apply independently; passing one never bypasses another. Project I/O
checks base64 length and padding before allocating decoded storage, accumulates aggregate limits
with checked arithmetic, and streams decoding into the already bounded destination.

## Manifest Shape

The canonical manifest has members in this exact order:

```json
{
  "format": "id.afterlab.bloom.project",
  "containerVersion": {
    "major": 1,
    "minor": 0
  },
  "document": {
    "path": "document.json",
    "schemaVersion": {
      "major": 1,
      "minor": 0
    }
  },
  "requirements": [],
  "generator": {
    "name": "Bloom",
    "version": "0.1.0"
  }
}
```

`format` and `document.path` are exact. The schema version repeated at the root of `document.json`
must equal the manifest declaration; disagreement is corruption.

`generator` records which application wrote the file: `name` is the application (`Bloom`) and
`version` the version the build is released under, the same string as its package files'
(`BLOOM_PACKAGE_VERSION`: the project version, or a pre-release's own such as `0.1.0-alpha.1`).
Every save writes it, last. It is informational only:
reading accepts a manifest without it, and nothing decides how to read, render or save a project
from it. A present generator holds exactly `name` then `version`, each a non-empty string of at most
256 UTF-8 bytes; anything else is a malformed manifest. Save Copy republishes the opened bytes, so a
copy keeps the generator of the file it copies.

A requirement record has members in this order:

```json
{
  "providerId": "vendor.module",
  "capabilityId": "vendor.module.project-data",
  "schemaVersion": {
    "major": 1,
    "minor": 0
  },
  "providedNodeTypeIds": [
    "vendor.module.node-type"
  ]
}
```

Requirements are sorted by `providerId`, then `capabilityId`, then schema major/minor. Duplicate
provider/capability pairs are invalid. Within each requirement, `providedNodeTypeIds` is sorted by
UTF-8 bytes and contains no duplicate. It may be empty when the capability owns only opaque
extension data. A requirement names data capability needed to interpret some project truth; it does
not persist an installed package version, download source, Python import path, or native ABI.
Opening never installs, enables, imports, migrates, or executes a provider.

The foundation node-type set is derived from the built-in node-definition registry. It includes
`bloom.composition-source` and the built-in structural, media, shape and value nodes. These types
are exempt from requirement coverage and must not appear in `providedNodeTypeIds`. Composition
sources store their references and time mapping as ordinary parameters; they add no document field.
For every other
distinct `NodeRecord.typeId` in the complete project, exactly one requirement lists that exact ID.
Conversely, every listed node type must occur in at least one `NodeRecord`; therefore the union of
all lists is exactly the project's distinct non-foundation node-type set. Missing coverage,
overlapping coverage, an unused listed type, or a listed foundation type is invalid. This explicit
manifest map determines unavailable node ownership without changing the live `NodeRecord` shape or
guessing from a string prefix.

An opaque extension owner is covered by at least one requirement with the same `providerId`.
Multiple capabilities from that provider may exist, but the exact capability needed by each known
extension type is declared by its registered format definition. Unavailable records retain the
opened requirement set through `RoundTripState`; Bloom never infers their capability from payload
bytes.

Project I/O owns the manifest requirement set alongside `RoundTripState`. Known registered document
types declare the requirement they introduce; requirements from an unavailable provider are
retained from the opened manifest. Before writing, Project I/O recomputes node-type coverage and
rejects a retained requirement that would make the exact-coverage invariant false. A writer removes
a known requirement only after no surviving node, extension record, or other durable construct
needs it. New projects using only foundation types and no extension-owned truth write an empty
array.

## Canonical JSON Primitives

The reader accepts insignificant RFC 8259 whitespace; typed decode requires known members in
their exact canonical order, consistent with the exact-order shape sections and with the
rejection of non-canonical spellings elsewhere in this contract — a reordered known member is a
typed decode error, not input to silent normalization, and the retained-unknown-member rules
depend on this order significance. The writer emits
every non-empty object and array over multiple lines, with one member or array element block per
level, two-space indentation, a single space after `:`, and commas only after non-final members or
elements. Empty containers are `{}` and `[]`. Output uses LF line endings, one final LF, and no
trailing whitespace. Small fragments below may compact whitespace to show type shape; golden files,
not compact fragments, demonstrate canonical layout.

### Decimal Strings And JSON Integers

- Every typed 64-bit ID is an unsigned decimal string. Valid object IDs use `[1-9][0-9]*` and must
  fit `uint64`; zero is invalid.
- Inclusive allocator high-water values use `0|[1-9][0-9]*` and must fit `uint64`.
- Every signed 64-bit semantic integer, including an `int64` parameter value and rational
  component, is a decimal string matching `0|-?[1-9][0-9]*` and must fit `int64`. `+0`, `-0`, a
  leading plus, and leading zero are invalid.
- Schema integers stored as JSON numbers are non-negative base-10 integers without a fraction or
  exponent and must fit their declared `uint32` or smaller range. JSON negative zero is invalid for
  an integer field.
- A rational object has `numerator`, then `denominator`. Its denominator is positive, the pair is
  already reduced, and zero is represented only as `{ "numerator": "0", "denominator": "1" }`.

Non-canonical integer and rational strings are rejected rather than silently normalized.

### Float64

Float fields are JSON numbers that round correctly to a finite IEEE 754 binary64 value. Overflow to
infinity is invalid. The writer starts from the [RFC 8785]/ECMAScript binary64 serialization, then
applies two type-preserving rules:

- positive zero is `0.0` and negative zero is `-0.0`
- when the representation contains neither a decimal point nor an exponent, append `.0`

All other digits, exponent thresholds, lowercase `e`, and exponent sign follow that serialization.
This preserves signed zero and the exact binary64 value while remaining independent of locale. The
reader may accept another finite RFC 8259 decimal spelling and normalizes it on save.

#### Non-Finite Values

NaN and the infinities have no spelling in this format, in any field, at any version. The invariant
is enforced at both ends and never by terminating:

- **Writing.** Every Float64 the document writer emits passes one admission point. A non-finite
  value fails the encode with `CanonicalDocumentError::NonFiniteValue` and the offending field's
  document path (for example
  `project/Composition[7]/AnimationCurve[12]/Keyframe[4]/value`), carried through
  `SaveArchiveDocumentEncodingFailure::fieldPath` at `SaveArchiveStage::DocumentEncode`. The save
  fails, the file on disk is untouched, and the session stays open and editable so the artist can
  correct the value; the UI names it ("Save failed: `<field>` is not a finite number"). Save, Save
  As and Save a Copy share the executor and therefore this behavior.
- **Reading.** `parseKnownFloat64` refuses a non-finite result, including a decimal that overflows
  to infinity, with `DocumentDecodeError::InvalidFloat64`. JSON has no NaN or infinity literal, so
  no well-formed archive can carry one.
- **Before either.** The document model refuses non-finite values on admission (`ParameterStore`,
  and `AnimationCurveStore` insert/update on every scalar and component key) and
  `Document::commit()` admits only a draft proven valid (see **Commit Validation And Publication
  Cost**), so a committed document cannot hold one. The writer's refusal is the last line of defence, not the first.

### Unknown JSON Numbers

An unknown additive member can remain editable only when Project I/O can preserve its JSON number
without guessing the absent owner's numeric type. The v1 lossless subset is deliberately narrow:

- a canonical integer token matching `0|-?[1-9][0-9]*` whose mathematical value fits `int64`; or
- the exact canonical Float64 spelling defined above, including the required `.0` for an integral
  finite value and preservation of signed zero.

`RoundTripState` stores the first as an exact `int64` and the second as its exact binary64 bits.
Canonical writing therefore reproduces the same mathematical value and the same canonical token
without retaining a general raw-number spelling. An unknown number outside this subset—including
an out-of-range integer, a non-canonical exponent or decimal spelling, or a decimal that would
round or normalize on rewrite—cannot be overlaid safely. Open returns `PreservedReadOnly` for that
archive rather than silently changing the unknown value. Known schema fields continue to follow
their declared integer or Float64 decoding rules.

### Strings And Object Keys

The writer preserves the decoded Unicode scalar sequence without NFC, NFD, case, or path
normalization. It emits ordinary non-ASCII scalars directly as UTF-8, does not escape `/`, uses the
short escapes `\b`, `\t`, `\n`, `\f`, and `\r`, and uses lowercase `\u00xx` for other control
characters below U+0020. `"` and `\` are escaped.

Duplicate-key detection compares decoded Unicode strings, so `"a"` and `"\u0061"` collide. Member
ordering compares valid UTF-8 bytes without locale or case folding.

## Canonical Document Shape

`document.json` has these members in exact order:

```json
{
  "schemaVersion": {
    "major": 1,
    "minor": 0
  },
  "project": {},
  "idAllocation": {
    "highestIssued": {}
  },
  "extensions": []
}
```

Known members use the order below. Retained unknown additive members follow all known members of
their object in ascending UTF-8 key order.

Document revision, command history, selection, current time, interaction overrides, workspace and
panel layout, caches, thumbnails, proxies, compiled plans, task state, and runtime capability state
are not project truth and are absent from `document.json`.

### Project And Composition

| Object | Required members in writer order |
| --- | --- |
| project | `id`, `name`, `colorSettings`, `compositions` |
| composition | `id`, `name`, `duration`, `format`, `parameters`, `animationCurves`, `graph`, `nodeLayout`, `nodeGroups`, optional `safeAreas`, optional `workArea` |
| composition format | `width`, `height`, `pixelAspect`, `frameRate` |
| rational/pixel aspect/frame rate | `numerator`, `denominator` |

Compositions are sorted by numeric `CompositionId`. `duration` is a positive normalized rational.
`width` and `height` are JSON-number `uint32` values satisfying the document extent limits.
`pixelAspect` and `frameRate` use canonical decimal-string rational objects and must also satisfy
their owning unsigned domain and normalization rules.

The schema fixes the live value domains exactly:

| Value | Accepted domain |
| --- | --- |
| `width`, `height` | integer `1..1048576` inclusive |
| composition pixel count | checked `uint64(width) * uint64(height) <= 4294967296` (`2^32`) |
| pixel-aspect numerator/denominator | reduced positive `uint32`, each `1..4294967295` |
| frame-rate numerator/denominator | reduced positive `uint32`, each `1..4294967295` |
| duration numerator | signed-64 decimal component `1..9223372036854775807` |
| duration denominator | signed-64 decimal component `1..9223372036854775807` |

Duration is reduced before serialization and must already be reduced on v1 decode. A zero or
negative duration, a zero unsigned-rational term, an unreduced pair, a dimension above the maximum,
or an extent whose checked product exceeds `2^32` is invalid. Project I/O performs product and
conversion checks before constructing `CompositionFormat` or `RationalTime`; it never narrows a
JSON value first.

### Project Color Settings And OCIO Reference

Version 1 requires one project-level `colorSettings` object. Its known members are written in this
exact order:

```json
{
  "schemaVersion": { "major": 1, "minor": 0 },
  "processColorSpaceId": "lin_rec709_scene",
  "ocioConfig": {
    "schemaVersion": { "major": 1, "minor": 0 },
    "locator": {
      "kind": "builtin",
      "uri": "bloom://ocio/neutral-v1/config.ocio"
    },
    "expectedRevision": {
      "algorithm": "sha256",
      "digest": "0000000000000000000000000000000000000000000000000000000000000000"
    },
    "portability": "builtin",
    "contextVariables": []
  }
}
```

The built-in URI above is exact; the all-zero digest illustrates the 64-hex-character shape only and
must be replaced by the qualified build profile's real revision. `processColorSpaceId` is owned by
`colorSettings` and is the selected scene-linear working-space identifier; a composition may
override it with its own `workingColorSpaceId`.
`expectedRevision` is owned by `colorSettings.ocioConfig`; it identifies the referenced
configuration bytes and never replaces or duplicates the process-space ID. The process encoding is
therefore known even while the referenced OCIO configuration is missing. Changing a display, view,
look, or monitor does not alter either durable value; those selections are session or render-request
state.

Each composition may additionally carry `workingColorSpaceId`. It is omitted for inheritance, or is
an exact scene-linear color-space id exposed by the project's selected OCIO config. The document
codec validates its UTF-8/length shape; the color boundary validates that the chosen config resolves
the id. A missing or non-scene-linear id fails closed rather than being replaced with the project
value. The effective id is written into evaluation intent and output identity, while the project
config revision remains the shared configuration identity.

`OcioConfigReference` has `schemaVersion`, `locator`, `expectedRevision`, `portability`, then
`contextVariables`. The locator is exactly one of these known-member shapes:

```json
{ "kind": "builtin", "uri": "bloom://ocio/neutral-v1/config.ocio" }
{ "kind": "project-relative-ocioz", "path": "color/config.ocioz" }
{ "kind": "external-ocioz", "uri": "file:///show/config.ocioz" }
{ "kind": "external-config", "uri": "file:///show/config.ocio" }
```

- The only schema `1.0` built-in locator is the exact ASCII URI
  `bloom://ocio/neutral-v1/config.ocio`. It has no user information, query, or fragment and resolves
  only through Bloom's qualified immutable registry. Mutable aliases, including any “default” or
  “latest” entry, are invalid durable values. The qualified adapter must return that same concrete
  URI before the reference is published.
- A `project-relative-ocioz` path is non-empty normalized UTF-8 with `/` separators, has an
  `.ocioz` suffix, and contains no absolute root, drive prefix, backslash, NUL, empty component,
  `.`, or `..`. It is at most 4096 UTF-8 bytes and resolves beside the `.bloom` file, never inside
  the project ZIP.
- An external URI is an absolute [RFC 8089] `file` URI with no user information, query, fragment,
  or NUL. `external-ocioz` names an `.ocioz` file; `external-config` names `config.ocio`. The URI is
  at most 16384 ASCII bytes, is preserved as authored, and resolves only through the cross-platform
  file-URI adapter.

`portability` must be `builtin`, `project-relative`, or `external` and must agree respectively with
the locator families above. The required revision record is written as `algorithm`, then `digest`;
`algorithm` is exactly `sha256` and `digest` is exactly 64 lowercase hexadecimal ASCII characters.
`OcioConfigReference` schema `1.0` fixes the revision serialization defined under **OCIO Content
Revision Version 1** in [`color-management.md`](color-management.md). For the built-in and `.ocioz`
families, the digest is SHA-256 over the domain prefix `BloomOcioRevision\0`, unsigned big-endian
version `u16(1)`, locator-kind tag, payload byte count, and exact built-in `config.ocio` or complete
archive bytes. For an external loose config, it is SHA-256 over the distinct domain prefix
`BloomOcioLooseRevision\0`, `u16(1)`, and the sorted canonical resource table: entry count followed
by each normalized root-relative UTF-8 key and exact payload bytes with the specified big-endian
length prefixes. That table contains `config.ocio` and every permitted resolved resource. Unicode
15.1 NFC, path rejection, collision handling, no-follow reads, and mutation checks are part of that
normative color contract; a hash over only `config.ocio`, raw filesystem enumeration order, or an
informal concatenation is invalid. Filesystem metadata and OCIO's runtime cache ID never replace
Bloom's versioned, domain-separated content revision.

A context-variable record has `name`, then `value`. Names match
`[A-Za-z_][A-Za-z0-9_]{0,127}`, values contain no NUL and are at most 4096 UTF-8 bytes, and the array
contains at most 256 records sorted by UTF-8 name with no duplicate. It is a complete explicit set:
ambient environment variables, the `OCIO` environment variable, current working directory, and
process-global configuration never supplement it during evaluation.

Syntactically valid but missing, changed, or unavailable known references remain known project
truth and are preserved unchanged; they diagnose and block only color-dependent processing. An
unknown color-settings, OCIO-reference, or locator discriminator prevents trusted color semantics
and therefore opens as `PreservedReadOnly`. Safely additive unknown members on known shapes follow
the ordinary `RoundTripState` rules. Container `1.0` never embeds an `.ocioz`: reference-only color
identity preserves the exact two-entry ZIP profile.

### Parameters

A parameter record has `id`, `schemaKey`, then `source`. Parameters are sorted by numeric
`ParameterId`.

Sources have these exact shapes:

```json
{ "kind": "constant", "value": {} }
{ "kind": "animation-curve", "curveId": "1" }
{ "kind": "driver", "sourceNodeId": "4", "outputPort": "value" }
```

Those are the only parameter-source discriminators. An unknown source kind encountered through a
newer compatible minor is unknown core semantics: Bloom preserves the bounded archive as
`PreservedReadOnly` rather than constructing a lossy placeholder or rewriting the source.

Every parameter source the document layer can hold is writable. A `DriverBindingSource` is the
durable `{sourceNodeId, outputPort}` pair it addresses, so the writer describes it the way it describes an edge source, and there is no parameter-source admission pass left
to fail: a driver's own well-formedness -- a valid node id and valid structural port text -- is already
refused by `ParameterStore` on insert, so a second check here could only ever be unreachable.

Constant values have one of these shapes and member orders:

```json
{ "kind": "bool", "value": true }
{ "kind": "int64", "value": "-1" }
{ "kind": "float64", "value": 1.0 }
{ "kind": "vec2", "x": 0.0, "y": 0.0 }
{ "kind": "color4", "red": 0.0, "green": 0.0, "blue": 0.0, "alpha": 1.0 }
{ "kind": "string", "value": "text" }
{ "kind": "rational", "numerator": "0", "denominator": "1" }
```

The parameter schema owns the semantic kind, color encoding, alpha meaning, range, and animation
capability. The decoder rejects a value/source kind that the named schema does not accept; it never
converts between variants by convenience.

### Animation

Animation curve records are sorted by numeric `AnimationCurveId` and have one of these shapes:

```json
{
  "id": "1",
  "kind": "scalar",
  "keyframes": []
}
```

```json
{
  "id": "2",
  "kind": "vec2",
  "keyframes": []
}
```

```json
{
  "id": "3",
  "kind": "color4",
  "keyframes": []
}
```

A scalar key has `id`, `time`, `value`, `outgoingInterpolation`, then the two OPTIONAL ease
handles described under **Ease Handles**; a `vec2` key replaces `value`
with an object containing `x`, then `y`; a `color4` key replaces it with an object containing `red`,
`green`, `blue`, then `alpha` -- the same authoring order and straight-alpha meaning a constant
`color4` parameter value already uses, with `alpha` confined to `[0, 1]`. Interpolation is
`hold`, `linear`, `ease-in-out`, or one of `sine`, `quad`, `cubic`, `quart`, `quint`, `expo`,
`circ`, `back`, `bounce`, `elastic`, and a key may end with the optional `easing` object (see
**Easing And Layer Flags**). Component keys are described under **Component Keyframes** and the
curve kinds under **Animation Values**.

Keys are written in strictly increasing exact rational time. Equal normalized times are invalid, so
`KeyframeId` never breaks a tie. Curves are non-empty; the final key's interpolation is canonical
`linear`. Curve kind, ownership, values, ranges, source references, and single-owner rules follow
[`animation-and-time.md`](animation-and-time.md).

### Canonical Graph

The graph object has `nodes`, `edges`, `layerOutputs`, `layerStack`, then `compositionOutput`.

| Object | Required members in writer order |
| --- | --- |
| node | `id`, `typeId`, `schemaVersion`, `parameters` |
| parameter binding | `role`, `parameterId` |
| edge | `id`, `source`, `destination` |
| output-port reference | `nodeId`, `port` |
| node-input destination | `kind`, `nodeId`, `port` |
| layer-stack destination | `kind`, `stackNodeId`, `slotId`, `role` |
| layer output boundary | `nodeId`, `layerId`, `name`, `outputPort` |
| layer stack | `nodeId`, `entries` |
| layer stack entry | `slotId`, `layerId` |

Destination `kind` is exactly `node-input` or `layer-stack-input`. Nodes sort by numeric `NodeId`,
edges by numeric `EdgeId`, and layer-output boundaries by numeric `LayerId` then `NodeId`. Parameter
bindings sort by UTF-8 `role` then numeric `ParameterId`. Layer Stack entries remain in semantic
top-to-bottom authoring order and are never ID-sorted. `compositionOutput` is a required member: an
output-port reference, or `null` for a composition whose Output node was deleted (it renders, plays
and exports nothing until an Output node is added again).

## Inclusive Allocator State

`idAllocation.highestIssued` has these required members in exact order:

```json
{
  "composition": "0",
  "node": "0",
  "edge": "0",
  "layer": "0",
  "layerSlot": "0",
  "parameter": "0",
  "animationCurve": "0",
  "keyframe": "0",
  "driverBinding": "0",
  "extensionRecord": "0"
}
```

Durable IDs are project-global within each typed namespace. Equal numeric values in different typed
namespaces are valid; duplicate declarations in the same namespace across compositions are not.
Validation rejects every cross-composition collision before initial `Document` construction or
publication.

Each value is inclusive:

- `0` means the namespace has never issued an ID and its next allocation is `1`
- a value below `uint64` maximum makes the next allocation `highestIssued + 1`
- `18446744073709551615` means that maximum was issued and the namespace is permanently exhausted

The allocator namespaces are exactly, in order, `composition`, `node`, `edge`, `layer`,
`layerSlot`, `parameter`, `animationCurve`, `keyframe`, `driverBinding`, `extensionRecord`,
`nodeGroup`, `asset`, the optional `assetFolder`, `dataBlock`, the optional `layerMask`,
`exportJob`, and `exportOutput`. No other member is accepted as a known namespace. Every
high-water value is an unsigned decimal string in `0..18446744073709551615`. Every declared object
ID is an unsigned decimal string in `1..18446744073709551615`. The document's sole `ProjectId` has
the same nonzero typed-ID domain but is not allocator-backed.

`driverBinding` remains required even though no driver declaration table is serialized and no
record carries a `DriverBindingId` at all -- a driver source is the node-and-port
pair it addresses. The live allocator may have issued driver IDs before that model; Save preserves
that inclusive high-water so reopen can never reuse one, and a nonzero `driverBinding` high-water with
nothing referencing it is valid and expected.

Every serialized typed ID in a namespace must be nonzero and at most its high-water value. The
high-water may exceed all surviving declarations because deleted and undone allocations remain
consumed. Commit, restore, undo, redo, clone, import, and reopen may increase but never lower it.

### Commit Validation And Publication Cost

Status: accepted (#357). Every published state is valid -- construction and every commit prove it
-- so a commit only has to prove what its draft changed relative to the published state it was
drafted from (`Project::validatesAgainst`). A composition is changed when its content identity or
any of its settings differ. Only changed compositions run `Composition::validate()`;
project-wide ID uniqueness compares their declarations with each other and with the frozen,
sorted per-composition ID index every published composition carries; composition nesting reads
each composition's cached Composition Source references; assets, folders and the project name are
revalidated only when their storage changed; extension records and data blocks are revalidated
when their storage changed, or when any composition changed while records exist that may name
composition content. Anything this cannot prove falls back to the whole `Project::validate()`,
which also supplies the refusal's diagnostics, so an invalid draft reports exactly what it always
did. `Draft::validate()` keeps returning the full diagnostics, warnings included; `Draft::valid()`
is the incremental yes/no. Diagnostic paths in project-wide ID checks and data-block rows are built
only when a diagnostic exists.

Assets, folders, extension records and data blocks are copy-on-write like composition content:
snapshots share one immutable collection, a draft copies a collection only when it edits it, and
publication freezes only the copies a draft made, so a retained draft pointer can never reach a
snapshot. Publication reserves allocator IDs only for changed compositions and edited root
collections; undo/redo restores rely on the historical state's own allocator, which already
covered its project. Published composition content also carries a lock index (one reverse search
from locked layers) and its render reachability, both built once at freeze time. Work counters
(`Project::incrementalValidationWork()`, `Composition::lockIndexBuilds()`,
`RenderReachability::computations()`) let tests prove an edit's cost does not grow with unrelated
compositions.

`ProjectId` is not allocator-backed because one container owns one project. A document missing any
required high-water member, including `driverBinding`, is invalid; high water is never
reconstructed from surviving IDs.

## Opaque Extension Envelope

An extension record is format-independent document truth with a project-global
`ExtensionRecordId`. Records are sorted by numeric record ID and serialize with these members:

```json
{
  "id": "1",
  "ownerId": "vendor.module",
  "typeId": "vendor.module.record-type",
  "schemaVersion": { "major": 1, "minor": 0 },
  "subject": null,
  "mediaType": "application/octet-stream",
  "referencePolicy": { "kind": "none" },
  "payload": "AA=="
}
```

`subject` is always present and is either `null` or `{ "kind": ..., "id": ... }`. Supported kinds
are `project`, `composition`, `node`, `edge`, `layer`, `layer-slot`, `parameter`,
`animation-curve`, and `keyframe`. Its typed target must exist in current document truth; an orphan
subject is invalid. A driver-binding subject is not part of v1 because v1 declares no driver table
or driver-binding subject semantics. The allocator watermark reserves the `DriverBindingId`
namespace without making driver bindings writable project records.

`payload` is canonical [RFC 4648] base64 using the standard alphabet, required `=` padding, and no
whitespace or line breaks. Decoded bytes are preserved exactly. `mediaType` describes those bytes;
the document model never depends on JSON or a provider-specific C++ type.

Reference policy has one of three exact forms:

```json
{ "kind": "none" }
```

```json
{
  "kind": "host-table",
  "references": [
    {
      "key": "primary-layer",
      "target": { "kind": "layer", "id": "1" }
    }
  ]
}
```

```json
{
  "kind": "owner-remapper",
  "remapperId": "vendor.module.record-remapper",
  "version": { "major": 1, "minor": 0 }
}
```

- `none` declares that the opaque payload contains no Bloom durable IDs.
- `host-table` requires payload schemas to refer through stable table keys rather than embedding
  raw Bloom numeric IDs. Keys are unique and sorted by UTF-8 bytes. Targets use the same typed kinds
  as subjects and must exist. Bloom remaps only table targets.
- `owner-remapper` declares that the bytes may embed Bloom IDs and require the named versioned
  provider remapper. Clone or import is rejected when that exact compatible remapper is unavailable.

Bloom never scans or rewrites opaque payload bytes heuristically. Duplicate records, invalid owner
or type IDs, orphan targets, invalid base64, incompatible reference-policy members, and payload
limit violations are invalid document state. An unavailable owner does not invalidate an otherwise
well-formed envelope; Bloom preserves it and diagnoses only affected editing/evaluation paths.

## Ordering Summary

Canonical ordering uses semantic values, never serialized decimal-string lexical order:

| Collection | Order |
| --- | --- |
| requirements | provider ID, capability ID, schema version |
| one requirement's `providedNodeTypeIds` | UTF-8 node-type ID |
| OCIO context variables | UTF-8 name |
| compositions | numeric `CompositionId` |
| assets | numeric `AssetId` |
| asset folders | numeric `AssetFolderId` |
| asset tags | UTF-8 tag text |
| parameters | numeric `ParameterId` |
| animation curves | numeric `AnimationCurveId` |
| curve keys | exact rational time |
| nodes | numeric `NodeId` |
| edges | numeric `EdgeId` |
| layer output boundaries | numeric `LayerId`, then `NodeId` |
| parameter bindings | UTF-8 role, then numeric `ParameterId` |
| Layer Stack entries | semantic authoring order |
| extension records | numeric `ExtensionRecordId` |
| host reference entries | UTF-8 key |
| retained unknown object members | UTF-8 key after known members |

## Versions And Preservation

Container and document versions use independent `{major, minor}` values:

- there is one current schema, container `1.0` and document `1.0`; nothing converts a file from
  another version
- unknown major versions are rejected without mutation
- a newer minor opens editable only when every unknown construct is additive, bounded, and
  provably preservable
- unknown core discriminators are never guessed
- unavailable optional providers preserve their neutral extension records and unknown node type IDs
  and block only dependent behavior

Declared provider data and accidental future JSON are different mechanisms:

1. Opaque extension records are document truth and participate in snapshots, validation, commands,
   undo, clone/import policy, and allocator state.
2. Unknown additive JSON members remain owned by Project I/O in move-only `RoundTripState`; they do
   not enter `Document` or acquire authoring semantics.

Round-trip reconciliation follows exact rules:

- known writer fields always replace retained values with the same key
- unknown members on singleton objects attach by schema path
- unknown members on collection objects attach by the collection's declared stable identity, never
  array position
- deleting a known object drops its attached unknown members; a newly allocated object inherits
  none
- an unknown array element is editable only when the owning schema declares a stable identity
  extractor and proves that preserving it cannot change known semantics
- structural clone/import is rejected when retained unknown data cannot be remapped safely
- whitespace, original escape spelling, known-number spelling, and original member order are not
  retained; an unknown number is editable only through the lossless typed subset above

Array reconciliation identities are fixed: requirement provider/capability pair; composition,
parameter, curve, keyframe, node, edge, and extension record typed ID; binding role within its node;
layer ID for a Layer Output; slot ID for a Layer Stack entry; and key for a host reference-table
entry. An array without a declared identity cannot retain unknown elements through an edit.

If all unknown data is safely additive, Project I/O overlays the current known document onto the
retained DOM and writes canonical JSON. If an unknown core discriminator prevents trusted document
construction, Open returns a preserved-read-only result containing the bounded original archive and
diagnostics rather than putting raw JSON into document truth. Native Save and Save As are disabled;
only byte-preserving Save Copy is available.

No v1.0 file may contain unknown ZIP members. Support for a later minor with additional members
requires RoundTripState to retain those entries within explicit byte and metadata limits first.

Open editability is explicit:

- `Editable` has trusted core semantics and all capabilities needed by current authoring paths.
- `DegradedEditable` has a trusted `Document` and a complete preservation proof, but unavailable
  providers or safely additive unknown members restrict only affected commands/evaluation.
- `PreservedReadOnly` cannot construct trusted complete core truth; it retains the original archive
  and permits inspection or byte-preserving Save Copy only.

### Node Version Validation

A node's `schemaVersion` is independent of the document schema. Built-in nodes must match their
registry definitions: Solid v2, Text v2, Layer v5, and Merge v2. Other built-in kinds keep their
registered versions. Retired per-kind reroutes are unsupported; only `bloom.reroute` is current.
Unknown optional-module kinds keep the preservation contract.

Project I/O checks node versions before decoding the rest of the document. An unsupported node
fails with one `UnsupportedNodeVersion` error carrying its `nodeTypeId`, `nodeVersion`, and exact
schema-version path. Reconstruction applies the same registry validation to decoded envelopes and
names the node ID. Old or future versions of a known kind are never silently upgraded, downgraded,
or interpreted through another definition. No parameters or IDs are injected and no edges are
dropped.

A node may leave any optional parameter role unbound. The compiler lowers an unbound optional role
at its definition default, so a node without, for example, a Transform 2D motion-blur, Film Grain
Monochrome or Sensor Noise, Procedural Warp split, Path Stroke stamp or Glow mode binding renders as
a node bound at those defaults.

## Project I/O Boundary

The public surface remains concrete rather than becoming a generic serializer. The conceptual
values are:

```cpp
struct DecodedProject {
    std::unique_ptr<document::Document> document;
    RoundTripState roundTrip;
    FormatVersion sourceVersion;
    Editability editability; // Editable or DegradedEditable
    std::vector<Diagnostic> diagnostics;
};

struct PreservedReadOnlyProject {
    PreservedArchive archive;
    FormatVersion sourceVersion;
    std::vector<Diagnostic> diagnostics;
};

using OpenedProject = std::variant<DecodedProject, PreservedReadOnlyProject>;

OpenResult openProject(const std::filesystem::path& path, const OpenOptions& options);

SaveResult saveProject(const std::filesystem::path& path,
                       const document::Snapshot& snapshot,
                       const RoundTripReadView& roundTrip,
                       const SaveOptions& options);
```

The codec API is synchronous internally and runs only through the task system or a headless worker
context. Open/Save options carry resource limits, cooperative cancellation, progress, expected
target state, and diagnostics without depending on Qt. The UI thread never parses, serializes,
compresses, resolves paths, opens files, flushes, waits, or drains I/O work.

## Staging And Atomic Publication

Saving uses the application `PublicationCoordinator` for intent order and a move-only
`src/platform::StagedArtifactLease` from the shared
`src/platform::StagedArtifactCoordinator` for secure staging and publication:

1. resolve the existing parent directory and canonical target identity off the UI thread
2. reject a symlink target leaf, a non-regular existing target, or a missing/non-directory parent
3. securely create a unique same-directory regular stage with exclusive create semantics
4. encode the archive, close it, reopen it, run the complete bounded Open/decode validation, and
   compare decoded known truth and retained-data proof with the captured save input
5. flush the staged file
6. re-check the expected external-file fingerprint and winning application-wide
   `PublicationIntentId` for the canonical target
7. enter the non-cancellable publication lease and atomically replace or create the target
8. flush the parent directory using the strongest supported parity-qualified primitive
9. clean staging data on every pre-publication exit; cleanup failure is a secondary diagnostic

The lease records the securely created stage's filesystem identity. Every reopen uses no-follow
semantics and verifies that identity, file type, and same-directory ownership before using or
publishing it.

Canonical target identity is the resolved parent-directory filesystem identity plus the leaf name
under that platform's native comparison semantics. An existing target's file identity and
fingerprint are additional conflict evidence. Resolution follows parent symlinks once but never
follows a target-leaf symlink. Different spelling, case aliases, and Save As paths that resolve to
the same target share one application intent-order record and one platform publication lease.

### Recovery Files

A session that is dirty periodically publishes its canonical document to a recovery file under the
platform per-user application data directory
(`Bloom/recovery/recovery-<owner UUID>-<session id>.bloom`), by default every 60 seconds.
The UUID is generated for each installed project content lifetime and is independent of the
process-local session ID, which starts again at one on each launch. New and successful Open install
fresh UUIDs, including when Open installs a recovery archive as the active project. This is not a
save and never claims to be one:

- it is written through the same nine-step staged/atomic sequence above, reusing the same archive
  writer and the same schema version, so a recovery file IS an ordinary project archive and
  recovering is just opening it
- no savepoint is accepted: the session's dirty state, revision and path authority are unchanged,
  and the artist still owes the project a real Save
- every failure is typed and non-fatal, and the next interval retries; a recovery write is never
  allowed to be the reason a session is lost
- a successful Save or an explicit unsaved-change Discard retires only the current installed
  content's file; a clean timer tick also removes only that file, except while it is the target of
  an accepted recovery Open

On the next launch, a recovery file is offered back ("Recover unsaved changes") only when it is
newer than the project it belongs to, or when that project was never saved at all. A recovery file
older than its saved project describes work the artist already has, and offering it would invite
them to overwrite the newer file with it. Discovery includes earlier
`recovery-<session id>.bloom` files. A pending or declined recovery offer remains on disk; even a
timer firing inside the decision dialog cannot retire an artifact owned by another launch.

The expected external-file fingerprint is either `Absent` or the existing regular file's identity,
byte size, high-resolution modification time, and SHA-256 of its complete bytes. Open computes it
while already streaming the archive; Save rechecks it on the blocking-I/O worker immediately before
publication. Size/time are fast rejection evidence and the hash detects content replacement. This
does not remove the final cross-process time-of-check/time-of-use window.

Platform publication behavior is:

- Linux and other POSIX hosts flush the stage, atomically rename it, then flush the parent directory
- macOS uses the same model with the strongest qualified full-file durability primitive
- Windows flushes the stage and uses `ReplaceFileW` for an existing target or a same-volume atomic
  move for first publication, then requests directory durability where supported

The result is exactly one of:

| Outcome | Contract |
| --- | --- |
| `Published` | target now names the validated new archive and durability steps succeeded |
| `PublishedWithDurabilityWarning` | replacement occurred, but a subsequent durability step failed |
| `Superseded` | a newer same-target publication intent won before this lease began |
| `CancelledBeforePublication` | cancellation won before this lease began |
| `ExternalModificationConflict` | target no longer matches the caller's expected state |
| `FailedBeforePublication` | target was not replaced |

Cancellation is observed through every stage boundary and bounded encode/decode loop before the
publication lease. After lease entry, publication is not cancellable and a late cancellation never
relabels a visible replacement as cancelled. If replacement succeeds but parent-directory flush
fails, Bloom cannot claim the previous file survived; it reports
`PublishedWithDurabilityWarning`.

Same-process project saves, Save Copy operations, and frame exports are serialized by canonical
target through the application-wide `PublicationCoordinator` defined in
[`project-session.md`](project-session.md); the platform `StagedArtifactCoordinator` owns their
shared staging and lease mechanics. External modification detection is required before replacement
but cannot eliminate the final cross-process race without a future portable locking protocol;
Bloom does not claim otherwise.

## Asset Locators

The document stores asset paths alongside stable records in `Project::assets`:

- nodes reference stable asset IDs, never filesystem paths
- project-relative paths use UTF-8 and `/` separators
- an absolute [RFC 8089](https://www.rfc-editor.org/rfc/rfc8089) file URI is the stored path for an
  explicit external file locator and remains the fallback hint for a project-relative locator
- relative paths resolve against the `.bloom` file's parent directory
- saving does not case-fold asset paths or resolve their symlinks
- embedded entry paths obey the container path profile when a later version introduces them

The closed image asset records are specified under **Images And Backgrounds**.

## Qualified Implementations

Initial qualification targets are:

- [libzip 1.11.4] under BSD-3-Clause for ZIP reading and writing
- [zlib 1.3.2] under the zlib license for the permitted deflate and CRC path
- [yyjson 0.12.0] under MIT for strict JSON, exact integers, correctly rounded doubles, mutable DOMs,
  and bounded custom allocation

They are private implementation dependencies pinned by release archive and cryptographic hash.
Headers are external/SYSTEM, optional compression, crypto, tool, and test features are disabled,
and license/provenance records enter notices and the software bill of materials. yyjson permits
duplicate keys structurally, so Bloom performs recursive decoded-key rejection.

Qualification must prove which malformed local/central-header, overlap, trailing-data, attribute,
and ZIP64 cases libzip exposes. Bloom adds a bounded raw preflight for any invariant the library
cannot enforce. Dependency versions and writer implementation are not persisted format identities.

Acquisition, lock, offline build, provenance, and release evidence follow
[`dependency-intake.md`](dependency-intake.md).

## Required Verification

- deterministic manifest/document bytes from repeated and cross-platform writes
- exact member and collection order, especially numeric IDs `1`, `2`, `9`, `10`, and `uint64` max
- every allocator namespace at fresh, deleted-gap, maximum, exhausted, and below-declaration states
- signed integer extrema, normalized rational extrema, finite/subnormal Float64 values, both signed
  zeros, exponent boundaries, and invalid overflow
- Unicode scalar, combining/non-normalized, astral, control, escaped-key collision, malformed UTF-8,
  and lone-surrogate fixtures
- scalar/Vec2/Color4 animation, the `ease-in-out` token, curve/key ownership, final-key
  normalization, graph connectivity, Layer Stack order, the `vec3` constant and `driver`
  parameter-source kinds, a live driver source encoding as the node-and-port
  pair it addresses, retained `driverBinding` high-water with no record referencing it, and
  source/schema mismatches
- exact composition-domain boundaries for dimensions, pixel-count product, positive normalized
  frame rate and pixel aspect, and signed-64 positive duration rationals
- exact, sorted, unique `providedNodeTypeIds` coverage for unavailable node types, including
  omissions, overlaps, extras, and foundation-type rejection
- project color-settings ordering, fixed `lin_rec709_scene` process identity, every OCIO locator
  family, locator/portability mismatch, alias rejection, digest spelling, context ordering, and
  reference-only `.ocioz` handling
- opaque record subject/reference policies, absent remappers, orphan targets, invalid base64, and
  per-record spelling/decoded and aggregate spelling/decoded payload limits
- unknown members at singleton and keyed-array depth across edit, reorder, delete, and new-object
  reconciliation; unknown numeric subset boundaries and unknown core discriminators produce
  preserved read-only state
- duplicate/aliased/traversal names, header disagreement, CRC damage, overlap, executable/special
  attributes, encryption, unsupported methods, descriptors, ZIP64, comments, polyglots, excessive
  depth/count/size/ratio, and decompression bombs
- checked operation and shared resident-memory charges at exact, one-byte-over,
  arithmetic-overflow, dependency-reservation, concurrent-task, and allocation-failure boundaries;
  all exhaustion paths preserve active state and invoke unpublished-stage cleanup, with cleanup
  faults reported secondarily
- fault injection at stage create, write, close, reopen, validation, flush, target-conflict,
  supersession, replace, directory flush, and cleanup
- cancellation before every publication boundary and cancellation after lease entry
- shared golden archives open and preserve identical document truth on Linux, macOS, and Windows

[ISO/IEC 21320-1]: https://www.iso.org/standard/60101.html
[RFC 8259]: https://www.rfc-editor.org/rfc/rfc8259
[RFC 8785]: https://www.rfc-editor.org/rfc/rfc8785
[RFC 4648]: https://www.rfc-editor.org/rfc/rfc4648
[RFC 8089]: https://www.rfc-editor.org/rfc/rfc8089
[libzip 1.11.4]: https://libzip.org/documentation/
[zlib 1.3.2]: https://github.com/madler/zlib/releases/tag/v1.3.2
[yyjson 0.12.0]: https://github.com/ibireme/yyjson/releases/tag/0.12.0


## Node Layout

Each composition has a required `nodeLayout` member after `graph`. It is an array sorted by numeric
`nodeId`; each record has exactly `nodeId`, `position` (`x`, `y`), `width`, `collapsed`, and
`muted`, in that order. Position and width are finite Float64 values; width is positive. The
booleans default to false. Unknown-node records survive with document warning diagnostics. Layout
identity never advances node allocation watermarks. Future-minor additive members on layout records
and their positions use normal round-trip preservation. Color-setting, OCIO, node, container, and
extension schema versions are independent of the document schema version.

## Node Groups

Each composition has a required `nodeGroups` member after `nodeLayout`. It is an array sorted by
numeric `groupId`; each record has exactly `groupId`, `name`, `members`, and `padding` (`x`, `y`),
in that order, then the optional `color` (see "Node Group Colors"). `members` is an array of node
object ids sorted ascending and duplicate-free, `name` is a human-facing name, and both padding
components are finite and nonnegative. Membership disjointness across groups and member existence
are document-model validation rather than wire shape: a member naming an unknown node survives with
a warning diagnostic, and a node claimed by two groups is a document error. Group identity never
advances node allocation watermarks. Future-minor additive members on group records and their
padding use normal round-trip preservation. Group ids come from the `nodeGroup` allocator
namespace, the eleventh member of `highestIssued`, after `extensionRecord`.

## Animation Values

Animation curves have the kinds `scalar`, `vec2`, `vec3`, `color4`, `path` and `gradient`. A
`color4` key carries `red`, `green`, `blue`, then `alpha` in that order -- the authoring order and
straight-alpha meaning the constant `color4` parameter value uses. `red`, `green`, and `blue` are
any finite number, so negative and HDR channels survive exactly; `alpha` is confined to `[0, 1]`,
the authoring-color contract a constant color satisfies. The `outgoingInterpolation` tokens are
`hold`, `linear`, `ease-in-out` and the easing equations of "Easing And Layer Flags", accepted on
every curve kind's keys.

## Timeline Fields

After a layer boundary's first four members, optional known members appear in this order:
`inPoint`, `outPoint`, `enabled`, `solo`, `locked`, `labelColor`. Times use the canonical rational
object. Zero `inPoint` and the zero full-duration `outPoint` sentinel are omitted by the writer.
Flags are omitted at their defaults (true, false, false respectively). A label is exactly three
JSON unsigned integers in `[0,255]`, in red/green/blue order; no alpha or color-space transform is
implied. A missing label uses the kind color. Unknown members still follow known members under
the additive-minor preservation rules.

After `nodeGroups`, a composition may contain `workArea`, with `start`, then `end` canonical
rational objects. Absence means `[0,duration)`; a present range satisfies
`0 <= start < end <= duration`. Layer ranges obey the same domain after resolving the duration
sentinel. Invalid ranges are refused during trusted document construction. The writer preserves
explicit ranges and work areas through close/reopen verification.

## Merge Collections

The `graph.layerStack` record holds the primary Merge's node ID and ordered slots; it is `null` only
when no Merge exists. Optional trailing `graph.merges` records carry additional Merges, each with
`nodeId`, ordered `entries`, and optional `enabled` (default true). Every Merge node owns one
collection. Entries carry `slotId` and `layerId`; `layerId: null` denotes a plain image input,
whose node and port come from its edge. An absent `merges` field means no additional Merges. Slot
IDs are project-unique and participate in allocator high-water validation. Output wiring
determines the timeline Merge independently of the primary persistence record.

## Content Bounds

Solid v2 has explicit Scalar width and height. Text v2, Layer v5, and Merge v2 use local content
bounds. These are the only supported definitions; documents carrying older node versions fail
validation. There is no coordinate conversion or compatibility evaluator.

Composition-sized Solid dimensions use the exact proxy extent to avoid rounding an extra column
or row into local bounds.

## Viewer Safe Areas

A composition may carry an optional `safeAreas` object, before `workArea` when that member is
present. It contains `action`, then `title`, as finite normalized percentages in `(0, 1]`, with
`title <= action`. The canonical writer omits the default `action: 0.9`, `title: 0.8` pair and
emits authored non-default settings in the same position, so custom safe-area choices are durable
across save/reopen. Missing `safeAreas` means the default pair. Preset selection and custom edits
are ordinary undoable composition commands. These values affect viewer guide painting only: they
never alter render pixels, cached frames, exports, or evaluation.

## Component Keyframes

Vector and color animation records are flat scalar component keys. A `vec2` key names `x` or `y`;
a `vec3` key names `x`, `y`, or `z`; a `color4` key names `red`, `green`, `blue`, or `alpha`. Every
component key carries an exact rational `time` and `outgoingInterpolation`. Component timelines are
independently ordered, so a component can be keyed without creating sibling keys. An animated
parameter may carry an optional typed `source.defaultValue`; an unkeyed component samples that
default.

## Images And Backgrounds

The manifest reserves `bloom.image-source` as a built-in type. Container and node schema versions
are independent.

`project` carries `assets` after `compositions`; each composition carries `backgroundColor` after
its optional work area. Background is a four-number straight RGBA array,
default `[0.0, 0.0, 0.0, 1.0]`. It is viewer presentation metadata and does not fill export alpha.
The Viewer displays it, opaque, inside the composition frame behind transparent pixels (unless the
Viewer's Transparency Grid is on); changing it is not render-affecting.
The asset allocator is appended after `nodeGroup`; asset IDs follow the existing decimal-string
and never-reuse/high-water rules, including undo and redo.

Assets are sorted by ID. Their closed member order is `id`, `kind`, `locator`, `contentDigest`,
`interpretation`, `width`, `height`, `manifest`. Kind is `image` or `sequence`. The closed locator
order is `kind: "file"`, `portability` (`"project-relative"` or `"external"`), UTF-8 `path`,
absolute `relinkHint`. Project-relative paths forbid `:` and `\`; external paths are absolute
file URIs validated before use, never synthetic percent-encoded relative paths. The file URI
encodes UTF-8 bytes, a local drive as `file:///C:/...`, and a UNC share as
`file://server/share/...`. Invalid escapes, NUL, non-file schemes and relative paths are rejected.
The digest is lowercase SHA-256 of image bytes; a sequence digest hashes ordered decimal frame
numbers plus `:` and each member's lowercase digest (a stamped member's provisional identity, which
a verified digest recorded beside the stamp does not change; see "Sequence Member Stamps" and
"Verified Member Digests"). Interpretation holds integer `colorSpace`
(0 Auto, 1 sRGB, 2 Linear, 3 Raw), string `inputColorSpaceId`, and `alphaAssociation` (0 Straight,
1 Premultiplied). The string is written after the legacy integer and before `alphaAssociation`; an
empty string means Auto. A non-empty value is an exact non-data color-space id in the project's
selected OCIO configuration. New imports leave the legacy enum at Auto so format metadata and EXR
chromaticities remain automatic.

The manifest contains `pattern`, `padding`, decimal-string `first` and `last`, sorted `members`
(frame, locator, and a digest, a file stamp, or both), and explicit
decimal-string `gaps`. A still carries an empty manifest.
Sequence ranges and gaps must agree with their members. Dimensions and locator lengths are
validated before publication. Missing files do not invalidate the document or remove its assets.
`RemoveAsset` preserves referencing nodes so they can warn; Relink retains the stable asset ID.
Import, relink, removal, layer placement and background editing use ordinary command transactions.

## Audio Assets And Graph Sources

An Audio asset uses the same stable `AssetRecord` identity and file `AssetLocator` as
an image, with `kind: "audio"`, the existing content digest and interpretation envelope, and the
captured source descriptor fields `rate`, `channels`, `frames`, and exact rational `duration`.
Audio has no image dimensions or sequence manifest. WAV and MP3 imports probe through the bounded
audio adapter and persist the probe result; filesystem bytes, decoded planar float32 buffers and
waveform buckets remain derived runtime state. Missing audio remains a valid reference and is
relinkable without changing its `AssetId`.

The `bloom.audio-source` node stores the stable audio asset ID as a String parameter, its integer
start frame, and its linear level. Layer Output, Layer Stack and Composition Output have the typed
audio ports described in [`layer-graph-model.md`](layer-graph-model.md); these are ordinary graph
nodes and edges, not an opaque timeline extension. `AddAudioLayer` allocates and persists the source,
boundary, slot and edge identities atomically. An image-only document contains no audio edges.

## Ease Handles

A scalar keyframe and a component keyframe may each carry `outgoingHandle` and `incomingHandle`,
in that order, after `outgoingInterpolation`. Each is a CLOSED object of `time` then `value`, both
Float64: `time` is a fraction of the adjoining segment's duration measured from the key that owns
the handle and is confined to `[0, 1]`; `value` is a finite offset from that key's own value. A
handle outside that domain is `InvalidKeyframeHandle`, the same predicate the document store and
the sampler apply, so a file can never carry a handle the model would refuse. The handle object is
not a round-trip attachment point: an unknown member inside one is `UnknownMember`.

Both members are OPTIONAL and are emitted ONLY when the handle is non-default -- the default being
`time = 1/3` and `value = 0`, tested BITWISE; see [`animation-and-time.md`](animation-and-time.md),
"Ease handles", for what the handles mean. A handle offsets one scalar axis, so only the scalar,
component, Path and Gradient keyframe definitions carry the members, through the schema's
`keyframeHandle-1.12` definition.

## Layer Parenting

A Layer Output may append `parent` after `labelColor`, before retained unknown members.
Its value is the canonical decimal-string LayerId of another boundary in the same composition.
Absence means no parent and is omitted by the writer. Null, zero, unknown or cross-composition
identities, self-parenting and parent cycles are refused. Parenting changes transform space only;
it does not change Merge membership, stack order, opacity or visibility.

## Font IDs And Text Layout

Text fonts are stored as font IDs ([Fonts](fonts.md)); a project holds no font bytes and the
writer emits no Font asset. Saving writes IDs only.

Text v2 appends `font` as a String holding the encoded font ID, then `box` as a Vec2 width/height in
pixels, `wrap` as a Boolean, `verticalAlignment`, `anchorMode`, and `overflow` as Integer selectors.
A zero box is point text; a non-zero box is the authored local bounds. The writer retains the
parameter sources in their stable graph order.

## Asset Organization

Organization metadata never changes media identity or evaluation. Each asset carries `name`,
optional `folder`, `tags`, then `order` after its media payload (`manifest`, and optional `font` or
`audio`). `name` is a non-empty display name of at
most 4,096 UTF-8 bytes. It is independent of the locator: rename never renames a file on disk.
`folder` is a non-zero decimal-string `AssetFolderId`; absence denotes the project root, and
`null` is not accepted. `tags` is an array of at most 64 non-empty UTF-8 strings, each at most
256 bytes, sorted uniquely by UTF-8 bytes. Command input is sorted and deduplicated; serialized
unsorted or duplicate tags are rejected. `order` is a canonical unsigned 64-bit decimal string,
including `"0"`, for the asset's position within its folder.

The project may carry `assetFolders` after `assets`. Its records are ordered by ID and have the
closed member order `id`, `name`, optional `parent`. Folder names use the same human-facing name
bound as assets; `parent` is another non-zero `AssetFolderId`, omitted for root folders. An empty
folder collection is omitted by the canonical writer. Folder IDs have their own allocator
namespace: `idAllocation.highestIssued` may carry `assetFolder` after `asset`, omitted only
when zero. Deletion and undo preserve this high-water mark, so identities are never reused.

Project validation refuses folder cycles, dangling parent/asset folder references, duplicate
folder identities and duplicate names among sibling folders. Sibling names compare exactly;
case-insensitive search is a UI behavior. Folders are virtual project organization, with no
filesystem directory creation or media relocation. A folder may also hold compositions (see
"Composition Folders"); removing a folder moves its assets, compositions and
child folders up to its parent.

The asset array stays sorted by stable ID for canonical encoding. The panel orders each folder's
assets by `(order, id)`, making equal imported positions deterministic. Move and reorder commands
write contiguous positions in affected folders; moving preserves the supplied selection order,
and its insertion index addresses the destination after removing the moving IDs. Reorder requires
a complete permutation of one folder's assets. Folder removal promotes direct child folders to
the parent and appends its assets in their existing order. If promotion would duplicate a sibling
folder name, removal is refused atomically until the conflicting folder is renamed.

Import derives display names lexically from locator file stems, preserves multiple dots before the
final extension and leaves dotfile names intact; an empty stem uses `Asset`. Relink keeps name,
folder, tags and order. No organization command changes render pixels.

## Layer Time

The document persists the layer-owned time model of
[ADR 0024](../decisions/0024-layer-owned-time.md). A Layer Output boundary in `layerOutputs` has
five optional members, written after `parent` in this order and only when they differ from the
identity default, so a layer without timing carries none of them:

| Member | Encoding | Default (omitted) |
| --- | --- | --- |
| `startTime` | rational-time object, same encoding as `inPoint` (any representable value, including negative) | `0` |
| `speed` | `{numerator, denominator}` decimal strings, reduced, positive, terms in `[1, 2^31 - 1]`, value in `[1/1000, 1000]` | `1/1` |
| `contentExtent` | `{"kind":"fixed","duration":<positive rational time>,"frameRate":<frame-rate object as in a composition format>}` or `{"kind":"source","node":<node id string>}` | Unbounded |
| `beforeMode`, `afterMode` | `"none"`, `"hold"`, `"loop"` or `"pingPong"` | `"none"` |

A present member is strict: a speed outside the range above, an unknown mode or
extent kind, or a fixed extent with a non-positive duration is a decode error rather than a silent
identity. The explicit layer range (`inPoint`, `outPoint`) may lie outside `[0, duration]`.

The Layer Output node (schema 5) declares the optional, animatable Float64 parameter role
`timeRemap` (schema key `bloom.layer.time-remap`). The parameter is not required, so a layer without
Time Remap has no binding or parameter record for it.

## Channel Sets

The document persists the multi-channel nodes of
[ADR 0026](../decisions/0026-multi-channel-and-data-images.md):

- `bloom.image-source` is at node schema 2. It has the optional String parameters `part`
  (`bloom.image.part`, an OpenEXR part name; empty is the first part) and `layer`
  (`bloom.image.layer`, the color layer; empty is the root layer), and a `channels` ChannelSet
  output beside `image`.
- `bloom.channel-shuffle`, `bloom.channel-copy` and `bloom.channel-remove` are at node schema 1. Their String parameters are bounded to 256 bytes (channel names) or 4096 bytes
  (patterns).
- The Composition Output has an optional `channels` input and an optional `layers` parameter
  (`bloom.composition-output.layers`) for layered OpenEXR output.
- `bloom.cryptomatte` (node schema 1, #364) has `channels` (ChannelSet) and `image`
  inputs, `mask` (Mask) and `image` outputs, and the String parameters `layer`
  (`bloom.cryptomatte.layer`, at most 256 bytes) and `mattes` (`bloom.cryptomatte.mattes`, the
  matte list in the common Cryptomatte syntax, at most 65536 bytes).

## Composition Timecode

A composition has an optional `timecode` member after `workingColorSpaceId` (#376):

| Member | Encoding |
| --- | --- |
| `timecode` | `{"start": <non-negative rational time>, "dropFrame": <boolean>}` |

`start` is the timecode of composition time 0, stored as an exact time so it survives a frame-rate
change; labels show it at the nearest frame of the composition rate. `dropFrame` asks for SMPTE
drop-frame labels and is honored only at 30000/1001 and 60000/1001; at any other rate the labels
are non-drop and the stored flag is kept. Neither value changes frames, time, evaluation or output
identity: they are timecode presentation owned by the composition. The member is written only when
it differs from the default (start 0, non-drop). The decoder refuses a negative start as a domain
violation and preserves unknown trailing members like any composition attachment.

## Node Group Colors

A node group object may carry an optional `color` after `padding`, before retained unknown members:
three integers in `0..255`, the frame's 8-bit sRGB tint, in the same shape as a Layer Output's
`labelColor`. The writer emits it only when the frame has a color; absent means the neutral frame. A
group's color is layout like the rest of the record: compilation, render caches and snapshot
identity never read it.

## Easing And Layer Flags

The document carries Robert Penner's easing equations
([animation-and-time.md](animation-and-time.md#easing-equations)) and two presentation-only layer
flags.

- Interpolation tokens `sine`, `quad`, `cubic`, `quart`, `quint`, `expo`, `circ`, `back`, `bounce`
  and `elastic` (ids 3-12; `hold`, `linear` and `ease-in-out` keep ids 0-2).
- A scalar, component or Path key may end, after its handles, with a closed `easing` object
  `{"mode", "back", "amplitude", "period"}`: `mode` is `auto`, `in`, `out` or `in-out`, and the
  three numbers are canonical Float64 (`amplitude` zero or more, all finite; `period` in seconds).
  It is written only when it differs bitwise from the default (`auto`, `1.70158`, `0.8`, `4.1/24`),
  so a key that never had its easing edited gains no member.
- A Layer Output may end, after `masks`/`trackMatte` and before retained unknown members, with
  `"shy": true` and then `"nameFollowsSource": true`; each is written only when true.

Neither flag is an evaluation input: `shy` only filters the layer table, and `nameFollowsSource`
only decides whether renaming or replacing the layer's source renames the layer (see
[layer-graph-model.md](layer-graph-model.md#layer-timeline-contract)).

## Sequence Member Stamps

Importing an image sequence lists its directory and reads one member: the file chosen, or the first
frame of an **Only frames** range that leaves it out (issue #473). That member's bytes are probed
for format, size, channels and alpha and hashed as before. Every other member is recorded with the
**file stamp** the listing observed, its length and modification time, and its bytes are read and
digested only when it is first needed (viewer, RAM Preview, export, thumbnails), on a worker.

A sequence `members` element carries `{frame, locator, contentDigest}`, `{frame, locator, stamp}`
or both, with `stamp` = `{size, modified}`: `size` an unsigned decimal string (the byte length) and
`modified` a signed decimal string (nanoseconds since the Unix epoch, as
`platform::observeFileIdentity` reports it on every platform). A project saved while only some
members have been read keeps the stamps of the others and reopens to the same asset. A save also
records a digest the process memo verified beside a member's stamp (see "Verified Member
Digests").

A stamped member's identity is provisional: `document::sequenceMemberIdentity` is SHA-256 over
`BloomSequenceMemberStamp`, NUL, the locator `path`, NUL, the `relinkHint`, NUL, the decimal size,
NUL and the decimal modification time. Path, size and time together, so frames of one size written
in one timestamp tick never share it. That identity stands in for the digest everywhere a digest
keys a cache (the asset's sequence digest, the evaluator's revision, decode and disk-cache keys,
the RAM Preview composite key and Assets thumbnails), so cache identity is correct before the bytes
are known and changes when the stamp does.

A stamped member's file is admitted only while it still carries its stamp. The first read observes
the file before and after reading it, refuses bytes whose stamp differs ("Frame 12 changed since it
was imported; reload the footage in Assets"; a missing file reads "Frame 12 is missing"), and
admits the bytes by their stamp without hashing them, so a later use costs one `stat`. The digest
is computed afterwards by the background Verify footage pass, which records it in the process memo
of [ADR 0025](../decisions/0025-identity-verified-media-sources.md) (see "Verified Member
Digests"). A revision retained in memory is reused only while the file still carries the
stamp and the memo still vouches for the retained bytes. Changed content is therefore never served stale. The
residual risk of ADR 0025 applies with one addition: a stamped member rewritten in place with the
same length and modification time is not detected, including across sessions, until Reload
Footage reads the sequence again. Copying media without preserving modification times makes
every stamped frame read as changed; Reload Footage (or Relink) lists it again.

A member whose bytes turn out not to be an image Bloom reads is found at its first read and follows
the unreadable-member policy of ADR 0026 / #373 then: it is reported ("Frame 12 is unreadable and is
treated as a gap: …") and shows what a gap shows, the preceding readable member (looking back at
most eight members), or nothing when the asset shows nothing for missing frames or no readable
member precedes it. A member that cannot even be stamped at import (missing, or not a regular file)
is a gap in the manifest, as before. Relink and Reload Footage list the sequence again the same way,
keeping its frame range; Relink to a single frame of a sequence still produces a still with the
digest of the file chosen.

## Gradient Colour Pairs

The Linear and Radial Gradient generators and Glow draw their colour ramps from gradient values
(issue #541):

- `bloom.linear-gradient` and `bloom.radial-gradient` are at node schema 2. Their required,
  animatable `gradient` role (`bloom.gradient.stops`, default black at 0 and white at 1) holds the
  ramp; gradient position 0 is drawn at the `start` point and 1 at the `end` point, and `repeat`
  repeats or mirrors the whole gradient.
- `bloom.glow` is at node schema 2. Its optional, animatable `tintGradient` role
  (`bloom.glow.tint-gradient`, default white at 0 and (1, 0.5, 0.25) at 1) holds the octave tints.
  With `gradient` on (and not Streak), octave `j` of `n` takes the gradient's colour at
  `j / (n - 1)`; `tint` stays the colour of every octave with the gradient off.

These gradients may hold any finite colour (HDR and signed colour stay authorable); Colorize's
keeps its `+-1e6` domain. A gradient as a whole has no socket (a gradient is not a socket kind);
each of its stops does (see "Gradient Stop Links"). A schema 1 generator or Glow is an unsupported
node version.

## Compound Paths

A path constant, Path key or Path data block may hold several contours, a fill rule and keyed groups
(issue #589, `docs/architecture/path-value.md`). A value a single contour can express keeps the
flat spelling, `{"kind": "path", "anchors": [...], "closed": ...}`. Any other value is written
`{"kind": "path", "contours": [{"anchors": [...], "closed": ...}, ...], "fillRule"?, "groups"?}`:
`fillRule` is `nonZero` or `evenOdd` and is written only when explicit, and each of `groups` is
`{"key", "firstContour", "contourCount", "color"?}`, written only when there are some, ascending and
not overlapping, with unique keys. A data block's path payload follows the same rule with its own
existing member order (`closed` before `anchors`). One value has one spelling, so a `contours` form
of a value the flat form can express is refused. Decoding gives each flat path one contour (an empty
open one gives none, the empty value). `tests/fixtures/paths` holds a saved project carrying
one-contour paths on shapes, a mask with keys, a data block, a field remap curve and a Curves node;
`bloom.runtime.path-value-project` opens it, requires every frame's digest, and saves and reopens
it. The schema holds the compound value as the `compoundPathValue-1.43` arm of
`parameterValue-1.4`.

## Number Lists

A number list constant (issue #627, `docs/architecture/number-list.md`) is written
`{"kind": "numberList", "values": [n, ...]}`: at most 65536 finite numbers in authored order, each
written as every other Float64 is. The empty list is `"values": []`. An item that is not a number,
a `values` that is not an array and a list past the limit are refused. No curve kind carries a
list, so the kind appears only as a constant. A build that predates the kind opens a project
holding one as preserved read-only, under the rule for unknown core discriminators. The schema
holds the value as the `numberListValue-1.44` arm of `parameterValue-1.4`;
`bloom.project.value-graph` saves, reopens and re-encodes one byte for byte.

## Matte Sources

A layer mask may follow its `opacity` with `matte`, an output port reference (`{"node", "port"}`)
naming the Mask output that shapes it (issue #383); its `path`, `feather` and `expansion` are still
stored. A track matte may carry `node`, the same reference, in place of `source`, followed by
`channel` and `inverted`. The referenced node must exist in the same composition, produce a Mask
and not depend on the layer. The schema holds `matte` on `layerMask-1.24`, and `trackMatte-1.24`
requires exactly one of `source` or `node`.

## Gradient Modes

A gradient value (a constant, and the value of every key of a gradient curve) may follow its
`stops` with how it blends between them (issue #546), in this order and each only when it is not
the default:

- `colorMode`: `"hsv"` or `"hsl"` (default RGB).
- `hueDirection`: `"far"`, `"clockwise"` or `"counter-clockwise"` (default Near).
- `interpolation`: `"constant"`, `"ease"`, `"cardinal"`, `"b-spline"` or `"exponential"` (default
  Linear).

A default spelled out, an unknown name or a member out of this order is refused. A mode the blend
does not read (the hue direction of an RGB gradient, the interpolation of an HSV or HSL one) is
still stored, so switching back restores it. The evaluation is evaluation-primitives.md's
"Gradient Interpolation And Colour Modes". The schema holds the three optional properties on
`gradientValue-1.38` and their `gradientColorMode-1.41`, `gradientHueDirection-1.41` and
`gradientInterpolation-1.41` vocabularies.

## Gradient Stop Links

A value node may drive a gradient stop's colour or position (issue #543), on Linear and Radial
Gradient's `gradient`, Glow's `tintGradient` and Colorize's `gradient`. Each driven component is its
own parameter, a stop link:

- Its schema key is `bloom.gradient.stop-color` (driven by a Color output) or
  `bloom.gradient.stop-position` (a Scalar output, or an Integer or Boolean one through the usual
  promotion), and its source is a driver (`{"kind": "driver", "sourceNodeId", "outputPort"}`) and
  nothing else.
- It is bound on the gradient's node under the stop's socket name,
  `<gradient role>.stops[<index>].color` or `.position` (`document::gradientStopPortName`), beside the
  gradient's own binding, which it needs. `<index>` is the stop's stored index, a decimal from 0 to
  63 with no leading zero; a stop link binds only a stop socket and a stop socket only a stop link.
- It is validated, ordered, cycle-checked and reached for rendering exactly as any driven
  parameter is: its socket kind is Color or Scalar, and a link from an undeclared output, from a
  sink or one that closes a loop is refused.

At each frame the gradient is sampled (its constant, or its curve at the frame) and each link then
sets its stop's colour, or its position clamped to `[0, 1]`; a link on a stop the gradient does not
have at that time changes nothing. A driven value that is not finite, or a colour whose alpha leaves
`[0, 1]`, makes the gradient not evaluable at that frame. The authored stop keeps its colour and
position under the link, and unlinking (DisconnectInput on the socket) removes the link record so
the stop renders as authored again. An edit that reorders or removes stops moves the links with
their stops in the same transaction (`commands::RemapGradientStopLinks`); a removed stop's links go
with it.

Stop links need no schema member of their own: parameters and bindings are generic.

## Gradient Values

A parameter constant may be a gradient (issue #541): `{"kind": "gradient", "stops": [...]}`, its
stops in stored order, each `{position, red, green, blue, alpha}` -- a position in `[0, 1]` and a
straight scene-linear colour exactly as a `color4` constant's. A gradient has 1 to 64 stops
(`document::kMaximumGradientStops`); stops may share a position and need not be sorted, and every
reader orders them by position with ties kept in stored order (`document::sortedGradientStops`).
The colour between two stops is the linear blend of their colours; before the first and after the
last the end colour holds. A missing, misordered or unknown member is refused like any constant's;
`stops` that is not an array or holds more than 64 stops is `InvalidConstantValueKind`; no stops, a
position outside `[0, 1]` or an invalid colour is `DomainViolation` at `stops`.

A gradient animates as one whole value, like a Path: an animation curve of kind `gradient` whose
keys carry the usual time, interpolation, easing and handle members and a `value` that is a
gradient constant. Between two keys with the same stop count each stop's position and colour
interpolate by the eased progress; keys with different stop counts, and Hold keys, hold the earlier
key's gradient until the next (`runtime::sampleGradientCurve`).

The schema keys with a gradient value are the generators' and Glow's (see "Gradient Colour Pairs")
and `bloom.colorize.gradient`, the Colorize node's optional `gradient` role. Its colour channels are
held to the Colorize colour domain; its default is black at 0, (0.24, 0.17, 0.10) at 0.18 and white
at 1. A new Colorize node binds it; a Colorize node without the binding maps through its Shadows,
Midtones, Highlights and Midpoint until the gradient is first edited, which binds it seeded with
that same ramp, so nothing changes on screen.

The schema defines `gradientStop-1.38`, `gradientValue-1.38` and `gradientKeyframe-1.38`, and holds
the gradient value in `parameterValue-1.4` and the gradient curve in `animationCurve-1.9`.

## Entry Layers

Every Merge entry is a layer (issue #502; see [the layer and graph model](layer-graph-model.md),
"Merge"). An entry without a Layer node keeps its layer controls on its own **entry layer**: a
`layerOutputs` record whose `nodeId` is the Merge and which appends, after `nameFollowsSource` and
before retained unknown members, in this order:

- `slotId`: the canonical decimal-string `LayerSlotId` of the Merge slot it belongs to. That slot's
  `layerId` names the record, and the slot is not fed directly by a Layer node.
- `pivot`: `"frame"`, written only when the layer's scale and rotation pivot about the composition
  frame's centre plus its anchor. Absence is the `content` pivot (the centre of its local
  content bounds). Every entry layer is created with the Frame pivot; a Layer
  node that adopted an entry's values carries it too, so it is written on any record. Any other
  value is refused.
- `parameters`: the entry layer's own parameter bindings, `{role, parameterId}` sorted by UTF-8 role
  then numeric id exactly as a node's are: `anchor`, `blendMode`, `opacity`, `position`, `rotation`,
  `scale`, and `timeRemap` only while Time Remap is enabled. Each parameter has the Layer node's
  schema key for its role, belongs to no node or other layer, and is a constant or an animation
  curve; never a driver. Only an entry layer writes it, never empty.

Layer-output records still sort by `layerId` then `nodeId`; the layer id is unique, so a Merge's
several entry layers stay canonical. A Merge may own one entry layer per slot; a Layer node still has
exactly one record. Validation (`document::validateEntryLayers`) refuses an entry layer whose slot is
missing or does not name it, two for one slot, a missing or repeated role, a parameter of the wrong
schema, a shared parameter and a driven one. A plain entry with no record behaves as an entry layer
at its defaults (the identity).

`bloom.adjustment` (schema 1) is the Adjustment node: no inputs, no parameters, one Image output
`image`, whose value is the stack below the Merge entry its chain lands in. Like every registered
node type it is admitted by the registry, not by the document version. The schema's
`layerOutput-1.37` record carries the three members.

## Verified Member Digests

A first use no longer hashes a still or a sequence member: the file identity check authorises
preview and export from first use, and the digest is computed in the background (issue #495). A
sequence `members` element takes one of three forms. `{frame, locator, contentDigest}` is a
**read** member (the member import read).
`{frame, locator, stamp}` is a **pending** member whose bytes no full hash has proved yet.
`{frame, locator, contentDigest, stamp}`, in that order, is a **verified** member: the digest a full
hash proved for the bytes that carried that stamp. Validation refuses a member with neither.

A member's identity does not move when its digest is recorded. `document::sequenceMemberIdentity`
is the stamp-derived identity of "Sequence Member Stamps" for every member with a stamp, digest or
not, and the digest itself for a read member. A read member that gains a stamp in a saved file is
keyed by its stamp once that file is reopened; within the session nothing changes.

**Save records, never waits.** Every publication of the document (Save, Save As, and the recovery
writer, which share `host::executeSessionSaveMiddle`) completes the captured snapshot on the save
worker, before serialization, through `host::snapshotWithVerifiedSequenceDigests`. For each image
sequence member lacking one of the two, it resolves the member's file the way the runtime does (the
locator under the project's folder, else its relink hint) and observes it once
(`platform::observeSource`: one `stat` and a lookup in the process memo of
[ADR 0025](../decisions/0025-identity-verified-media-sources.md)). A pending member gains the memo
digest when the file still carries the member's stamp; a read member gains the observed stamp when
the memo digest equals its digest; a different memo digest (the content changed) leaves it as it
is, as does a verified member, a missing file and any member the memo does not know. Nothing is
hashed and nothing waits: a member without a digest saves as pending. Only the published bytes
change; the live document, its revision, dirty state and undo history are not touched, and a failure
to complete the snapshot publishes the captured one. Save Copy republishes the retained archive
byte for byte and records nothing.

**Verify footage.** The Assets controller runs one background job per project, "Verify footage"
in **Jobs** (owner Project, `Background` priority, the blocking-I/O executor; phase "Verifying
footage", completed and total files; cancellable). It starts when a project is installed or opened
and after an import, Relink or Reload Footage commits; a newer request supersedes the running pass,
and project replacement and shutdown cancel it. It never queues ahead of an import, a save or an
open on the one blocking-I/O worker: one that starts meanwhile stops the pass at once (mid-file),
and the pass starts again when they finish, skipping what it already read. The hashing runs on a
thread of its own, which the worker waits for, at the operating system's idle priority
(`platform::IdleThreadPriorityScope`: on Linux `SCHED_IDLE` and the idle I/O class, applied
without privilege because the thread ends with the pass; the Darwin background state on macOS;
Windows background mode).

The pass hashes every still image (`platform::hashSourceFile`, which records the memo when the
file's identity holds still across the read), every pending member whose file still carries its
stamp, every read member, and a verified member whose file no longer carries its stamp. It skips
a verified member that still carries its stamp, any file the memo already knows for its current
identity, a missing file, and every video (a video's decode snapshot still hashes at capture) and
other asset kind. A hash that differs from the recorded digest adds a Warning naming the asset and
frame ("plate frame 12 changed since it was imported", "Relink or reload the footage in Assets.")
and purges the decoded media and finished-frame caches through the media purge of Edit | Purge, so
nothing rendered from the changed bytes stays cached; the memo then holds the actual digest, so a
later decode reports the relink diagnostic. The next save records every digest the pass proved.
There is no preference and no setting. The schema's `assetMember-1.36` requires at least one of
`contentDigest` and `stamp` (`anyOf`).

## Interpret Footage

An asset's `interpretation` object carries the Interpret Footage members (issue #381), after
`alphaAssociation` and in this order. Each is optional and written only when it differs from its
default, so an asset the artist never interpreted carries none of them:

| Member | Value | Default (absent) |
| --- | --- | --- |
| `frameRate` | `{numerator, denominator}`: a sequence's assumed rate or a video's conformed rate | a sequence advances one member per composition frame; a video plays at its file rate |
| `startTimecode` | `{start, dropFrame}`: the start timecode override as an exact time | the source's own timecode |
| `alphaMode` | `ignore`, `straight` or `premultiplied` | Guess: the association the file declared (`alphaAssociation`), with each image node's Premultiply switch in charge |
| `invertAlpha` | `true` | not inverted |
| `matteColor` | three numbers in [0, 1]: the color a premultiplied alpha was matted against, in the file's encoding | black |
| `fieldOrder` | `progressive`, `upperFirst` or `lowerFirst` | the field order captured at import |
| `deinterlace` | `true` | not deinterlaced |
| `pixelAspect` | `{numerator, denominator}` in [1/100, 100] | the file's ratio |
| `loopCount` | integer 2 to 1000: the footage plays this many times end to end | 1 |
| `missingFrames` | `nothing`: a frame the sequence manifest lacks shows nothing; `skip`: the sequence plays the frames it has back to back, so it is as long as its members | hold the previous frame |
| `localCopies` | `always` or `never`: whether Bloom keeps a local copy of this asset's source media in its disk cache, whatever the Preferences say; it changes no pixel and keys no cache | automatic: the Preferences and the storage the file is on decide |
| `stillDuration` | rational time, more than 0 and at most 24 hours: a still's length, from the Import preference **Still duration**; a layer made from the still ends there and New Composition from it is that long | the composition's length |

A sequence's `manifest` has an optional trailing `range` (`{first, last}`, signed decimal
frame numbers, `first <= last`): the numbered frames an import was limited to (the Import option
**Only frames**). Every member lies within it, and Reload Footage and Relink read only those frames
again; absent means every numbered file of the sequence.

A video stream has an optional trailing `pixelAspect` (`{numerator, denominator}`, never 1/1):
the coded sample aspect ratio the decoder resamples to square pixels, captured at import (provider
schema 5); absent means square. A still has no frame rate, start timecode, fields or loop, only
a sequence has missing frames and a frame range, and only a still has a still duration; validation
refuses them on other kinds. A member written at its default or an unknown token is refused.

## Export Formats

Three export output formats serve the bundled web encoders
([ADR 0036](../decisions/0036-bundled-codec-libraries.md)): `vp9-webm` (VP9 with Opus audio in
WebM), `av1-webm` (AV1 with Opus audio in WebM) and `av1-mp4` (AV1 with AAC audio in MP4). Their
`codecProfile` is empty (the format default, 8-bit) or one of `8bit` and `10bit`. `videoBitrateKbps` is kept but does not apply to them (they encode at
constant quality), and a WebM output's audio is mixed at 48 kHz whatever `audioSampleRate` says.
The schema holds the format and profile vocabularies in `exportFormat-1.33` and
`exportCodecProfile-1.33`.

## Keyframe Handle Modes

A key may carry a handle mode ([animation-and-time.md](animation-and-time.md#handle-modes)). A scalar, component or Path key may end,
after its handles and `easing`, with `"handleMode": "aligned"` or `"handleMode": "auto"`. `free` is the
default and is never written, so a key that never had its mode set gains no member; a file that spells
`"handleMode": "free"` is refused (`InvalidKeyframeHandleMode`), as is any other token.

The mode is presentation metadata: sampling reads the stored handles, and `evaluationView()` resets
the mode, so a transaction that changes only modes is render-neutral. A key without the member
is Free.

## Export Queue

The document persists the export queue ([ADR 0033](../decisions/0033-export-queue.md)): the jobs,
their render settings and their outputs are project truth, edited only through undoable commands
(`commands/export_queue_operations.hpp`) that never affect a rendered pixel. Run state (status,
progress, ETA, logs, verification, replace confirmations) is runtime state owned by
`host::ExportQueueService` and is never saved, so a reopened project shows every job Ready.

`project` has the required last member `exportQueue`: `{"jobs": [...]}` in run order (not sorted
by id). `idAllocation.highestIssued` has the required `exportJob` and `exportOutput` members after
the optional `layerMask`. Ids are allocator-backed decimal strings, project-global per namespace, and
never reused.

| Object | Members in writer order |
| --- | --- |
| job | `id`, `composition`, `name`, `range`, `outputs`, `enabled`, optional `labelColor` |
| range | `kind` (`"work-area"`, `"composition"`, `"custom"`, `"frame"`), `first`, and `last` only for `custom` (decimal strings) |
| output | `id`, `preset`, `settings`, `destination`, `enabled`, `importIntoAssets` |
| settings | `format`, `codecProfile`, optional `videoBitrateKbps`, `compression`, `outputColorSpace`, `display`, `view`, `bakeLook`, `includeAudio`, optional `useCachedFrames` (only `false` is written), optional `loudness`, `audioSampleRate`, `encoder` (`"automatic"`, `"software"`, `"hardware"`), `naming` |
| naming | optional `startFrame` (decimal string), `framePadding`, `pattern` |
| destination | `portability` (`"project-relative"`, `"external"`), `path`, `overwrite` (`"ask"`, `"replace"`, `"refuse"`) |

`loudness` (optional, #570) is present only when the output normalizes its programme loudness:
`targetLufs` (Float64, `-40..-5`, EBU R128 integrated loudness) and `truePeakDbtp` (Float64,
`-12..0`, the true-peak ceiling). Like `useCachedFrames` it is optional: an output without it
exports the audio as mixed.

`labelColor` uses the layer label representation (three integers `0..255`). `format` is the closed
vocabulary `png`, `exr`, `tiff`, `dpx`, `prores-mov`, `dnxhr-mxf`, `h264-mov`, `h264-mp4`,
`hevc-mov`, `hevc-mp4`, `wav`, `vp9-webm`, `av1-webm`, `av1-mp4`; `codecProfile` is empty (the format default) or a token the format
accepts (ProRes `proxy|lt|422|hq|4444|4444xq`, DNxHR `lb|sq|hq|hqx|444`, H.264 `high`, HEVC
`main|main10`, DPX `10bit|16bit`, WAV `s16|s24`, the WebM and AV1 formats `8bit|10bit`; see
"Export Formats"). The shapes are closed. Bounds: 1,024 jobs, 1 to 16
outputs per job, 4 KiB per string, frame padding 1 to 12, one `####` or `{frame}` in a naming
pattern. A job's composition must exist: deleting a composition removes its jobs in the same edit,
and undo restores both. A project-relative destination is a relative path below the project folder;
an external destination is an RFC 8089 file URI. Paths and patterns may contain the destination
tokens `{name}`, `{job}`, `{format}` and `{date}` (expanded by `host::resolveDestination()`).

## Video Color Default

No member records how Auto shows SDR video ([ADR 0032](../decisions/0032-sdr-video-colour-default.md),
issue #381): Auto shows ordinary Rec.709/601 and untagged SDR video through the config's sRGB
texture space instead of its Rec.709 video space (see
[color management](color-management.md#automatic-asset-rules)). Auto stays Auto and every explicit or
legacy input color is kept exactly, so no per-asset assumption is stored.

## Composition Folders

A composition has an optional `folder` member after `timecode` (#378):

| Member | Encoding |
| --- | --- |
| `folder` | the canonical decimal-string `AssetFolderId` of the Assets folder holding the composition |

Absence means the Assets root, and the writer omits it there. A folder reference is project
organization only: it never changes evaluation, render identity or caches. Project validation
refuses a composition whose folder does not exist, exactly as for an asset. Compositions carry no
order within a folder; the Assets panel sorts them with assets by its columns. The decoder preserves
unknown trailing members like any composition attachment.

## Layer Masks

The document persists the masks, track mattes and Path curves of
[ADR 0028](../decisions/0028-layer-masks-and-track-mattes.md). Every member is optional and written
only when present, so a document without masks carries none of them.

A Layer Output boundary has two members after the layer-time members:

| Member | Encoding |
| --- | --- |
| `masks` | non-empty array (at most 64) of `{"id", "name", "mode", "inverted", "path", "feather", "expansion", "opacity"}`: the mask id, a human-facing name, `"none"`, `"add"`, `"subtract"`, `"intersect"` or `"difference"`, a boolean, and the ids of the four parameters it owns (schema keys `bloom.mask.path`, `bloom.mask.feather`, `bloom.mask.expansion`, `bloom.mask.opacity`) |
| `trackMatte` | `{"source": <layer id>, "channel": "alpha" or "luma", "inverted": <boolean>}` |

A mask is a collection element identified by its id (round-trip kind `LayerMask`), so unknown
trailing members of a mask object are preserved like those of any other element. Mask parameters
are ordinary parameter records owned by exactly one mask and never bound by a node. A mask may take
its shape from a node's Mask output and a track matte may name a Mask output in place of its source
layer (see "Matte Sources").

A path anchor has an optional trailing `mode` member: `"corner"`, `"mirrored"`, `"aligned"`,
`"free"` or `"auto"`, the editing intent of its handles (it never changes a pixel). It is written
only when it differs from the mode the anchor's geometry implies, and an omitted mode is that
inferred mode: no handle is Corner, two collinear handles of equal length are Mirrored, two collinear
handles are Aligned, anything else is Free (relative tolerance 1e-9). The canonical form therefore
round-trips exactly. Data-block path payloads carry no mode. Path keys carry the mode on each anchor
of their value.

An animation curve may be `{"id", "kind": "path", "keyframes"}`. Each key is `{"id", "time",
"value", "outgoingInterpolation"}` plus the optional ease handles, where `value` is a constant-value
object of kind `"path"`. Only a `bloom.mask.path` parameter may own a Path curve.

`idAllocation.highestIssued` has an optional `layerMask` member, written once a mask id has been
issued.

## Video Assets

`AssetKind::Video` serializes as `"video"`. It retains the shared asset identity, locator,
interpretation, dimensions and organization envelope, with a closed `video` member before
organization fields. `video` contains `frames` (canonical unsigned decimal string), exact rational
`duration`, and a nonempty `streams` array (at most 256). A stream contains, in order: `id`, `kind`
(1 video, 2 audio, 3 data), `codec`, `profile`, `pixelFormat`, `timecode`, `timebase`, `framePeriod`,
`duration`, `width`, `height`, `sampleRate`, `primaries`, `transfer`, `matrix`, `range`,
`fieldOrder`, `timecodeType`, `channelLayout`, then the optional `pixelAspect` (see "Video Stream
Metadata" and "Interpret Footage"). Rates are stored as exact positive reciprocal frame periods.
Color code points use canonical signed decimal strings; absent tags remain explicit. Stream IDs
follow container order. Dimensions, strings, timing, audio layout and frame counts are bounded
during decoding and domain validation. Only video assets may carry this member.

The first video stream defines the asset's duration, frame count and dimensions. The first audio
stream supplies its preview sample rate and channel count. Content digest binds every decode and
cache lookup. ProRes authorization is not project truth: the current provider's explicit
non-authorized status is execution evidence and appears in the asset tooltip.

## Asset Input Color Spaces

Each asset interpretation carries `inputColorSpaceId`, and `bloom.image-source` and
`bloom.video-source` carry the matching `inputColorSpaceId` String parameter. Both source rows are
non-animatable. The source value is empty to inherit the asset; otherwise it is the exact config id
selected by the artist. The integer `colorSpace` rows remain: `0` inherits the asset's legacy
interpretation, while values `1` through `3` keep their sRGB, Linear, and Raw behavior. Properties
and the node card show one control, Input color space, for both rows; it offers Raw as "Raw (no
conversion)" and shows a stored sRGB or Linear choice without offering it, and committing writes the
OCIO id and clears the legacy choice.

The canonical writer emits `colorSpace`, `inputColorSpaceId`, then `alphaAssociation` in that
order. The field is bounded and validated as strict UTF-8. Missing, data, or unavailable ids do
not fall back to a similarly named space or to an ambient config; the asset remains diagnostically
unavailable until the user chooses a valid id.

`Rec.2100-HLG (Bloom)` is a valid persisted id under both pinned configs although neither config
file defines it: Bloom synthesizes it into the resolved config (HLG-OETF-INVERSE from OCIO 2.5,
scaled so the 75% signal is 1.0, then the BT.2020 to reference-primaries matrix; scene light, no
OOTF) whenever the config is resolved, so opening a project that names it never depends on the
config file. `Rec.2100-PQ (Bloom)` is likewise valid under Bloom Neutral (ST-2084 to linear, 203
cd/m2 = 1.0). See [`color-management.md`](color-management.md#synthesized-rec2100-hlg-input-space).

## Video Stream Metadata

Every video-asset stream object has two members after `range`, written by the canonical writer and
required by the decoder:

| Member | Values |
| --- | --- |
| `fieldOrder` | coded field order, FFmpeg numbering: 0 unknown, 1 progressive, 2 top first, 3 bottom first, 4 top coded/bottom displayed first, 5 the reverse |
| `timecodeType` | 0 none, 1 non-drop, 2 drop-frame (`;` or `.` before the frames field); 0 exactly when `timecode` is empty |

The Video node (`bloom.video-source`, schema 1) has the optional, non-animatable integer parameter
`bloom.video.audio-stream`, the 0-based index among the asset's audio streams that the node plays
(0 to 255). A node without the binding plays the first audio stream; the Properties Audio Stream
control creates the binding on first use through `bloom.video.set-audio-stream`.
