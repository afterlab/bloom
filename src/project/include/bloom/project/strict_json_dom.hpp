#pragma once

#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/project_io_memory_resource.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The bounded strict JSON DOM: a reader-side counterpart to the canonical JSON writer described
// in docs/architecture/project-format.md ("Canonical JSON Primitives" and "Versions
// And Preservation"). `parseStrictJsonDom()` first runs the existing bounded
// `bloom::project::detail::preflightStrictJson()` scan, then parses the accepted bytes with a
// qualified yyjson 0.12 reader whose allocation is bound to the caller's
// `ProjectIoOperationMemory` budget. No yyjson type, header, or macro appears here or in any
// other public Bloom contract; yyjson stays an implementation detail of strict_json_dom.cpp. The
// resulting tree is a move-only, Bloom-owned value type: object member order is preserved exactly
// as decoded, decoded duplicate object keys are rejected (comparing decoded Unicode strings, so
// `"a"` and `"a"` collide), and every JSON number keeps its exact source token untouched --
// typed numeric conversion (canonical decimal/rational/Float64, or the lossless unknown-number
// subset) remains the caller's responsibility using the existing primitives in
// canonical_decimal.hpp and unknown_json_number.hpp.

namespace bloom::project {

namespace detail {
class StrictJsonDomBuilder;
// The parsed document's storage (its PMR resource, the qualified reader's document, and the arena
// the tree lives in); defined in strict_json_dom.cpp so no reader type crosses this header.
struct StrictJsonDomStorage;
} // namespace detail

enum class JsonValueKind : std::uint8_t {
    Null,
    Boolean,
    Number,
    String,
    Array,
    Object,
};

class JsonValue;
class JsonMember;

// One decoded JSON value in the bounded strict DOM: 16 bytes, a view into storage its owning
// StrictJsonDomDocument holds. A number or string points at its text in the qualified reader's
// decoded buffer; an array's elements and an object's members are each one contiguous block in the
// document's arena. Nothing is allocated per value, so a document costs a small fixed amount per
// value on top of its text (issue #737: the former per-value strings and vectors cost over 150
// bytes per value and could not hold a large project within one save's memory budget). Copying is
// disabled so a tree is never duplicated by accident; a value is only valid while its document
// lives.
class JsonValue final {
  public:
    // A Null value; every other kind is populated by the parse pipeline.
    JsonValue() noexcept = default;

    JsonValue(const JsonValue&) = delete;
    JsonValue& operator=(const JsonValue&) = delete;
    JsonValue(JsonValue&&) noexcept = default;
    JsonValue& operator=(JsonValue&&) noexcept = default;
    ~JsonValue() = default;

    [[nodiscard]] JsonValueKind kind() const noexcept { return kind_; }
    [[nodiscard]] bool isNull() const noexcept { return kind_ == JsonValueKind::Null; }

    // Valid only when kind() == Boolean; nullopt otherwise.
    [[nodiscard]] std::optional<bool> asBoolean() const noexcept;
    // Valid only when kind() == Number. Returns the exact source token bytes (e.g. "-0", "1e10",
    // "9007199254740992.0"); the DOM performs no numeric conversion or normalization.
    [[nodiscard]] std::optional<std::string_view> asNumberToken() const noexcept;
    // Valid only when kind() == String. Returns the decoded Unicode scalar sequence (escapes
    // already resolved), never the original escaped spelling.
    [[nodiscard]] std::optional<std::string_view> asString() const noexcept;
    // Valid only when kind() == Array; empty otherwise.
    [[nodiscard]] std::span<const JsonValue> arrayElements() const noexcept;
    // Valid only when kind() == Object; empty otherwise. Members retain exact source order.
    [[nodiscard]] std::span<const JsonMember> objectMembers() const noexcept;
    // Linear lookup by decoded key; returns nullptr when kind() != Object or the key is absent.
    [[nodiscard]] const JsonValue* findMember(std::string_view key) const noexcept;

  private:
    friend class detail::StrictJsonDomBuilder;

    // The text of a Number or String, the first element of an Array, or the first member of an
    // Object; `size_` counts bytes, elements or members. Every count fits: a decoded string is at
    // most kStrictJsonDomMaximumDecodedStringBytes and a container at most
    // kStrictJsonDomMaximumContainerEntries (both checked before the tree is built).
    const void* data_ = nullptr;
    std::uint32_t size_ = 0;
    JsonValueKind kind_ = JsonValueKind::Null;
    bool boolean_ = false;
};

class JsonMember final {
  public:
    JsonMember() noexcept = default;

    JsonMember(const JsonMember&) = delete;
    JsonMember& operator=(const JsonMember&) = delete;
    JsonMember(JsonMember&&) noexcept = default;
    JsonMember& operator=(JsonMember&&) noexcept = default;
    ~JsonMember() = default;

    [[nodiscard]] std::string_view key() const noexcept { return {key_, keySize_}; }
    [[nodiscard]] const JsonValue& value() const noexcept { return value_; }

  private:
    friend class detail::StrictJsonDomBuilder;

    const char* key_ = nullptr;
    std::uint32_t keySize_ = 0;
    JsonValue value_;
};

inline constexpr std::size_t kStrictJsonDomMaximumInputBytes = 256U << 20U;
inline constexpr std::uint32_t kStrictJsonDomMaximumDepth = 128;
inline constexpr std::uint64_t kStrictJsonDomMaximumValues = 8'000'000;
inline constexpr std::uint64_t kStrictJsonDomMaximumContainerEntries = 1'000'000;
inline constexpr std::uint64_t kStrictJsonDomMaximumDecodedStringBytes = 89'478'488;

// Bounds applied by the preceding preflight scan and carried through to the DOM build. A field
// above the fixed v1 ceiling (mirroring strict_json_preflight.hpp) is rejected as InvalidLimits;
// callers only ever lower these budgets.
struct StrictJsonDomLimits final {
    std::size_t maximumInputBytes = kStrictJsonDomMaximumInputBytes;
    std::uint64_t maximumValues = kStrictJsonDomMaximumValues;
    std::uint64_t maximumContainerEntries = kStrictJsonDomMaximumContainerEntries;
    std::uint32_t maximumDepth = kStrictJsonDomMaximumDepth;
    std::uint64_t maximumDecodedStringBytes = kStrictJsonDomMaximumDecodedStringBytes;
};

enum class StrictJsonDomError : std::uint8_t {
    None,
    InvalidLimits,
    InputTooLarge,
    BomForbidden,
    EmptyInput,
    InvalidUtf8,
    InvalidSyntax,
    InvalidEscape,
    InvalidUnicodeScalar,
    InvalidNumber,
    TrailingData,
    DepthLimitExceeded,
    ValueLimitExceeded,
    ContainerEntryLimitExceeded,
    DecodedStringLimitExceeded,
    SizeOverflow,
    // A decoded object key collided with an earlier decoded key in the same object; memberPath()
    // names the exact offending member.
    DuplicateObjectKey,
    // The bounded PMR budget rejected an allocation needed by the qualified reader or the DOM
    // build; the destination document is never constructed.
    ResourceExhausted,
    // The qualified reader rejected input that the preflight scan accepted; this should not occur
    // given a conforming preflight/reader pair and is reported rather than trusted blindly.
    ParseFailed,
};

// A bounded diagnostic path to one JSON member, formatted as `/key/0/key`. Truncated() is true
// when the exact path did not fit; the retained prefix is still a genuine path prefix.
class StrictJsonDomPathText final {
  public:
    StrictJsonDomPathText() noexcept = default;

    [[nodiscard]] std::string_view view() const& noexcept { return {chars_.data(), size_}; }
    [[nodiscard]] std::string_view view() const&& = delete;
    [[nodiscard]] bool truncated() const noexcept { return truncated_; }

  private:
    friend class detail::StrictJsonDomBuilder;

    std::array<char, 512> chars_{};
    std::uint16_t size_ = 0;
    bool truncated_ = false;
};

// A parsed document owning its storage (PMR resource, decoded text and tree arena) and root value
// for as long as the document lives. The storage lives behind a stable heap address so moving the
// document never invalidates the views its tree holds.
class StrictJsonDomDocument final {
  public:
    StrictJsonDomDocument(const StrictJsonDomDocument&) = delete;
    StrictJsonDomDocument& operator=(const StrictJsonDomDocument&) = delete;
    StrictJsonDomDocument(StrictJsonDomDocument&&) noexcept;
    StrictJsonDomDocument& operator=(StrictJsonDomDocument&&) noexcept;
    ~StrictJsonDomDocument();

    [[nodiscard]] const JsonValue& root() const noexcept { return root_; }

  private:
    friend class detail::StrictJsonDomBuilder;

    StrictJsonDomDocument(std::unique_ptr<detail::StrictJsonDomStorage> storage,
                          JsonValue root) noexcept;

    std::unique_ptr<detail::StrictJsonDomStorage> storage_;
    JsonValue root_;
};

// [[nodiscard]] failure-aware result. On success, document() names the parsed tree; on failure,
// error() names the typed cause with either byteOffset() (preflight/reader-stage failures, an
// RFC 8259 byte position) or memberPath() (DuplicateObjectKey) populated.
class [[nodiscard]] StrictJsonDomResult final {
  public:
    StrictJsonDomResult(const StrictJsonDomResult&) = delete;
    StrictJsonDomResult& operator=(const StrictJsonDomResult&) = delete;
    StrictJsonDomResult(StrictJsonDomResult&&) noexcept = default;
    StrictJsonDomResult& operator=(StrictJsonDomResult&&) noexcept = default;
    ~StrictJsonDomResult() = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error_ == StrictJsonDomError::None;
    }
    [[nodiscard]] StrictJsonDomError error() const noexcept { return error_; }
    [[nodiscard]] std::size_t byteOffset() const noexcept { return byteOffset_; }
    [[nodiscard]] std::string_view memberPath() const noexcept { return path_.view(); }
    [[nodiscard]] const StrictJsonDomDocument* document() const& noexcept {
        return document_.has_value() ? &*document_ : nullptr;
    }
    [[nodiscard]] const StrictJsonDomDocument* document() const&& = delete;

  private:
    friend class detail::StrictJsonDomBuilder;

    StrictJsonDomResult() noexcept = default;

    std::optional<StrictJsonDomDocument> document_;
    StrictJsonDomError error_ = StrictJsonDomError::InvalidLimits;
    std::size_t byteOffset_ = 0;
    StrictJsonDomPathText path_;
};

// Runs the bounded preflight scan with `limits`, then parses the accepted bytes into a bounded
// Bloom-owned DOM. `operation` supplies and bounds every allocation made on this call's behalf,
// including the qualified reader's internal working memory; a budget rejection is reported as
// ResourceExhausted rather than throwing or falling back to unmetered heap allocation. Never
// throws.
[[nodiscard]] StrictJsonDomResult parseStrictJsonDom(std::span<const std::byte> input,
                                                     const StrictJsonDomLimits& limits,
                                                     ProjectIoOperationMemory operation) noexcept;

} // namespace bloom::project
