#include <bloom/project/strict_json_dom.hpp>

#include "strict_json_preflight.hpp"

#include <yyjson.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace bloom::project {

std::optional<bool> JsonValue::asBoolean() const noexcept {
    if (kind_ != JsonValueKind::Boolean) {
        return std::nullopt;
    }
    return boolean_;
}

std::optional<std::string_view> JsonValue::asNumberToken() const noexcept {
    if (kind_ != JsonValueKind::Number) {
        return std::nullopt;
    }
    return std::string_view{static_cast<const char*>(data_), size_};
}

std::optional<std::string_view> JsonValue::asString() const noexcept {
    if (kind_ != JsonValueKind::String) {
        return std::nullopt;
    }
    return std::string_view{static_cast<const char*>(data_), size_};
}

std::span<const JsonValue> JsonValue::arrayElements() const noexcept {
    if (kind_ != JsonValueKind::Array || size_ == 0) {
        return {};
    }
    return {static_cast<const JsonValue*>(data_), size_};
}

std::span<const JsonMember> JsonValue::objectMembers() const noexcept {
    if (kind_ != JsonValueKind::Object || size_ == 0) {
        return {};
    }
    return {static_cast<const JsonMember*>(data_), size_};
}

const JsonValue* JsonValue::findMember(const std::string_view key) const noexcept {
    for (const auto& member : objectMembers()) {
        if (member.key() == key) {
            return &member.value();
        }
    }
    return nullptr;
}

// Every count the tree stores fits its 32-bit field (see JsonValue's own comment).
static_assert(kStrictJsonDomMaximumDecodedStringBytes <= UINT32_MAX);
static_assert(kStrictJsonDomMaximumContainerEntries <= UINT32_MAX);
static_assert(kStrictJsonDomMaximumInputBytes <= UINT32_MAX);
// The per-value cost the project-format resource budget is sized against (issue #737).
static_assert(sizeof(JsonValue) <= 16);
static_assert(sizeof(JsonMember) <= 32);
static_assert(std::is_trivially_destructible_v<JsonValue>);
static_assert(std::is_trivially_destructible_v<JsonMember>);

} // namespace bloom::project

namespace bloom::project::detail {

// The document's storage. Declared in this order so it is destroyed in the reverse: the arena's
// chunks and the qualified reader's document are released first, then the chunk list, and the PMR
// resource that charged all of them last.
struct StrictJsonDomStorage final {
    explicit StrictJsonDomStorage(ProjectIoOperationMemory operation) noexcept
        : resource(std::move(operation)) {}

    StrictJsonDomStorage(const StrictJsonDomStorage&) = delete;
    StrictJsonDomStorage& operator=(const StrictJsonDomStorage&) = delete;
    StrictJsonDomStorage(StrictJsonDomStorage&&) = delete;
    StrictJsonDomStorage& operator=(StrictJsonDomStorage&&) = delete;

    ~StrictJsonDomStorage() {
        for (const auto& chunk : chunks_) {
            resource.deallocate(chunk.data, chunk.size, alignof(std::max_align_t));
        }
        if (document != nullptr) {
            yyjson_doc_free(document);
        }
        if (text != nullptr) {
            resource.deallocate(text, textCapacity, alignof(std::max_align_t));
        }
    }

    // A bump allocation of `count` default-constructed T in the arena. Throws std::bad_alloc when
    // the operation budget refuses a chunk, which parse() reports as ResourceExhausted.
    template <typename T> [[nodiscard]] T* allocateArray(const std::size_t count) {
        static_assert(std::is_trivially_destructible_v<T>);
        if (count == 0) {
            return nullptr;
        }
        const auto bytes = count * sizeof(T);
        auto* const storage = allocate(bytes, alignof(T));
        auto* const first = static_cast<T*>(storage);
        for (std::size_t index = 0; index < count; ++index) {
            ::new (static_cast<void*>(first + index)) T();
        }
        return first;
    }

    ProjectIoMemoryResource resource;
    // The reader's tree, released as soon as the Bloom tree is built from it.
    yyjson_doc* document = nullptr;
    // The decoded text every number, string and key of the tree points into: a padded copy of the
    // input that the reader decodes in place.
    char* text = nullptr;
    std::size_t textCapacity = 0;

  private:
    struct Chunk final {
        void* data = nullptr;
        std::size_t size = 0;
    };

    // Chunks grow from 4 KiB by doubling to 4 MiB, so a small document charges little and a large
    // one wastes at most a fraction of one chunk; a block larger than the cap gets its own exact
    // chunk. The same sizes on every platform, unlike std::pmr::monotonic_buffer_resource's
    // implementation-defined growth.
    static constexpr std::size_t kFirstChunkBytes = std::size_t{4} << 10U;
    static constexpr std::size_t kMaximumChunkBytes = std::size_t{4} << 20U;

    [[nodiscard]] void* allocate(const std::size_t bytes, const std::size_t alignment) {
        const auto padding =
            remaining_ == 0
                ? 0
                : (alignment - (reinterpret_cast<std::uintptr_t>(cursor_) % alignment)) % alignment;
        if (remaining_ < padding || remaining_ - padding < bytes) {
            if (bytes > kMaximumChunkBytes) {
                // Its own exact chunk; the current chunk keeps serving smaller blocks.
                return addChunk(bytes);
            }
            nextChunkBytes_ = std::max(nextChunkBytes_, bytes);
            cursor_ = static_cast<std::byte*>(addChunk(nextChunkBytes_));
            remaining_ = nextChunkBytes_;
            nextChunkBytes_ = std::min(nextChunkBytes_ * 2, kMaximumChunkBytes);
            auto* const block = cursor_;
            cursor_ += bytes;
            remaining_ -= bytes;
            return block;
        }
        auto* const block = cursor_ + padding;
        cursor_ = block + bytes;
        remaining_ -= padding + bytes;
        return block;
    }

    [[nodiscard]] void* addChunk(const std::size_t bytes) {
        chunks_.reserve(chunks_.size() + 1);
        void* const data = resource.allocate(bytes, alignof(std::max_align_t));
        chunks_.push_back({.data = data, .size = bytes});
        return data;
    }

    std::pmr::vector<Chunk> chunks_{&resource};
    std::byte* cursor_ = nullptr;
    std::size_t remaining_ = 0;
    std::size_t nextChunkBytes_ = kFirstChunkBytes;
};

} // namespace bloom::project::detail

namespace bloom::project {

StrictJsonDomDocument::StrictJsonDomDocument(std::unique_ptr<detail::StrictJsonDomStorage> storage,
                                             JsonValue root) noexcept
    : storage_(std::move(storage)), root_(std::move(root)) {}

StrictJsonDomDocument::StrictJsonDomDocument(StrictJsonDomDocument&&) noexcept = default;
StrictJsonDomDocument& StrictJsonDomDocument::operator=(StrictJsonDomDocument&&) noexcept = default;
StrictJsonDomDocument::~StrictJsonDomDocument() = default;

} // namespace bloom::project

namespace bloom::project::detail {

namespace {

static_assert(kStrictJsonDomMaximumInputBytes == kStrictJsonDocumentMaximumInputBytes);
static_assert(kStrictJsonDomMaximumDepth == kStrictJsonMaximumDepth);
static_assert(kStrictJsonDomMaximumValues == kStrictJsonMaximumValues);
static_assert(kStrictJsonDomMaximumContainerEntries == kStrictJsonMaximumContainerEntries);
static_assert(kStrictJsonDomMaximumDecodedStringBytes == kStrictJsonMaximumDecodedStringBytes);

[[nodiscard]] StrictJsonDomError
translatePreflightError(const StrictJsonPreflightError error) noexcept {
    switch (error) {
    case StrictJsonPreflightError::None:
        return StrictJsonDomError::None;
    case StrictJsonPreflightError::InvalidLimits:
        return StrictJsonDomError::InvalidLimits;
    case StrictJsonPreflightError::InputTooLarge:
        return StrictJsonDomError::InputTooLarge;
    case StrictJsonPreflightError::BomForbidden:
        return StrictJsonDomError::BomForbidden;
    case StrictJsonPreflightError::EmptyInput:
        return StrictJsonDomError::EmptyInput;
    case StrictJsonPreflightError::InvalidUtf8:
        return StrictJsonDomError::InvalidUtf8;
    case StrictJsonPreflightError::InvalidSyntax:
        return StrictJsonDomError::InvalidSyntax;
    case StrictJsonPreflightError::InvalidEscape:
        return StrictJsonDomError::InvalidEscape;
    case StrictJsonPreflightError::InvalidUnicodeScalar:
        return StrictJsonDomError::InvalidUnicodeScalar;
    case StrictJsonPreflightError::InvalidNumber:
        return StrictJsonDomError::InvalidNumber;
    case StrictJsonPreflightError::TrailingData:
        return StrictJsonDomError::TrailingData;
    case StrictJsonPreflightError::DepthLimitExceeded:
        return StrictJsonDomError::DepthLimitExceeded;
    case StrictJsonPreflightError::ValueLimitExceeded:
        return StrictJsonDomError::ValueLimitExceeded;
    case StrictJsonPreflightError::ContainerEntryLimitExceeded:
        return StrictJsonDomError::ContainerEntryLimitExceeded;
    case StrictJsonPreflightError::DecodedStringLimitExceeded:
        return StrictJsonDomError::DecodedStringLimitExceeded;
    case StrictJsonPreflightError::SizeOverflow:
        return StrictJsonDomError::SizeOverflow;
    case StrictJsonPreflightError::Cancelled:
        return StrictJsonDomError::ParseFailed;
    }
    return StrictJsonDomError::ParseFailed;
}

struct PathSegment final {
    bool isIndex = false;
    std::string_view key;
    std::size_t index = 0;
};

// A fixed-capacity stack of path segments from the document root to the value currently being
// built. Sized to the fixed v1 depth ceiling, which every accepted `StrictJsonDomLimits` is
// checked against before this stack is used.
class PathStack final {
  public:
    void pushKey(const std::string_view key) noexcept {
        entries_[size_] = PathSegment{.isIndex = false, .key = key, .index = 0};
        ++size_;
    }
    void pushIndex(const std::size_t index) noexcept {
        entries_[size_] = PathSegment{.isIndex = true, .key = {}, .index = index};
        ++size_;
    }
    void pop() noexcept { --size_; }
    [[nodiscard]] std::span<const PathSegment> segments() const noexcept {
        return {entries_.data(), size_};
    }

  private:
    std::array<PathSegment, kStrictJsonDomMaximumDepth> entries_{};
    std::size_t size_ = 0;
};

} // namespace

// All yyjson use is confined to this translation unit; no yyjson type crosses strict_json_dom.hpp.
class StrictJsonDomBuilder final {
  public:
    [[nodiscard]] static StrictJsonDomResult parse(std::span<const std::byte> input,
                                                   const StrictJsonDomLimits& limits,
                                                   ProjectIoOperationMemory operation) noexcept;

  private:
    // Not noexcept: the arena and the duplicate-key set allocate through ProjectIoMemoryResource's
    // throwing memory_resource surface (do_allocate). A budget rejection here must propagate as
    // std::bad_alloc to parse()'s try/catch, which reports ResourceExhausted; the allocator
    // adapters used only by yyjson's C callbacks are the noexcept ones (checkedAllocate), not this
    // walk. The tree's text is never copied: it points into the reader's decoded buffer, which the
    // document's storage keeps alive.
    [[nodiscard]] static bool buildValue(yyjson_val* value, JsonValue& out, PathStack& path,
                                         StrictJsonDomStorage& storage,
                                         StrictJsonDomPathText& errorPath);
    [[nodiscard]] static StrictJsonDomPathText formatPath(const PathStack& stack,
                                                          std::string_view finalKey) noexcept;

    [[nodiscard]] static StrictJsonDomResult makeFailure(StrictJsonDomError error,
                                                         std::size_t byteOffset) noexcept;
    [[nodiscard]] static StrictJsonDomResult makeSuccess(StrictJsonDomDocument document) noexcept;
    [[nodiscard]] static StrictJsonDomResult
    makeDuplicateKeyFailure(StrictJsonDomPathText path) noexcept;

    static void* allocatorMalloc(void* ctx, std::size_t size) noexcept;
    static void* allocatorRealloc(void* ctx, void* ptr, std::size_t oldSize,
                                  std::size_t size) noexcept;
    static void allocatorFree(void* ctx, void* ptr) noexcept;
};

StrictJsonDomResult StrictJsonDomBuilder::parse(const std::span<const std::byte> input,
                                                const StrictJsonDomLimits& limits,
                                                ProjectIoOperationMemory operation) noexcept {
    if (limits.maximumInputBytes > kStrictJsonDocumentMaximumInputBytes ||
        limits.maximumValues > kStrictJsonMaximumValues ||
        limits.maximumContainerEntries > kStrictJsonMaximumContainerEntries ||
        limits.maximumDepth > kStrictJsonMaximumDepth ||
        limits.maximumDecodedStringBytes > kStrictJsonMaximumDecodedStringBytes) {
        return makeFailure(StrictJsonDomError::InvalidLimits, 0);
    }

    const StrictJsonPreflightLimits preflightLimits{
        .maximumInputBytes = limits.maximumInputBytes,
        .maximumValues = limits.maximumValues,
        .maximumContainerEntries = limits.maximumContainerEntries,
        .maximumDepth = limits.maximumDepth,
        .maximumDecodedStringBytes = limits.maximumDecodedStringBytes,
    };
    const auto preflight = preflightStrictJson(input, preflightLimits);
    if (!preflight.succeeded()) {
        return makeFailure(translatePreflightError(preflight.error), preflight.errorOffset);
    }

    std::unique_ptr<StrictJsonDomStorage> storage;
    try {
        storage = std::make_unique<StrictJsonDomStorage>(std::move(operation));
    } catch (const std::bad_alloc&) {
        return makeFailure(StrictJsonDomError::ResourceExhausted, 0);
    }

    const yyjson_alc allocator{&allocatorMalloc, &allocatorRealloc, &allocatorFree,
                               &storage->resource};
    // The reader decodes in place (YYJSON_READ_INSITU) into a zero-padded copy the storage owns, so
    // the tree's text outlives the reader's own tree, which is freed once the Bloom tree is built:
    // the reader's 16 bytes per value and per key are not held while the document is decoded
    // (issue #737). `input` itself is never modified. NUMBER_AS_RAW keeps every number's exact
    // source token; the grammar is strict RFC 8259, matching the preceding preflight scan.
    try {
        storage->textCapacity = input.size() + YYJSON_PADDING_SIZE;
        storage->text = static_cast<char*>(
            storage->resource.allocate(storage->textCapacity, alignof(std::max_align_t)));
    } catch (const std::bad_alloc&) {
        storage->textCapacity = 0;
        return makeFailure(StrictJsonDomError::ResourceExhausted, 0);
    }
    std::memcpy(storage->text, input.data(), input.size());
    std::memset(storage->text + input.size(), 0, YYJSON_PADDING_SIZE);
    yyjson_read_err readError{};
    storage->document =
        yyjson_read_opts(storage->text, input.size(),
                         YYJSON_READ_INSITU | YYJSON_READ_NUMBER_AS_RAW, &allocator, &readError);
    if (storage->document == nullptr) {
        const auto error = readError.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION
                               ? StrictJsonDomError::ResourceExhausted
                               : StrictJsonDomError::ParseFailed;
        return makeFailure(error, readError.pos);
    }

    try {
        PathStack path;
        JsonValue root;
        StrictJsonDomPathText errorPath;
        if (!buildValue(yyjson_doc_get_root(storage->document), root, path, *storage, errorPath)) {
            return makeDuplicateKeyFailure(errorPath);
        }
        yyjson_doc_free(storage->document);
        storage->document = nullptr;
        StrictJsonDomDocument document(std::move(storage), std::move(root));
        return makeSuccess(std::move(document));
    } catch (const std::bad_alloc&) {
        return makeFailure(StrictJsonDomError::ResourceExhausted, 0);
    } catch (...) {
        return makeFailure(StrictJsonDomError::ParseFailed, 0);
    }
}

bool StrictJsonDomBuilder::buildValue(yyjson_val* const value, JsonValue& out, PathStack& path,
                                      StrictJsonDomStorage& storage,
                                      StrictJsonDomPathText& errorPath) {
    switch (yyjson_get_type(value)) {
    case YYJSON_TYPE_NULL:
        out.kind_ = JsonValueKind::Null;
        return true;
    case YYJSON_TYPE_BOOL:
        out.kind_ = JsonValueKind::Boolean;
        out.boolean_ = yyjson_is_true(value);
        return true;
    case YYJSON_TYPE_RAW:
        out.kind_ = JsonValueKind::Number;
        out.data_ = yyjson_get_raw(value);
        out.size_ = static_cast<std::uint32_t>(yyjson_get_len(value));
        return true;
    case YYJSON_TYPE_STR:
        out.kind_ = JsonValueKind::String;
        out.data_ = yyjson_get_str(value);
        out.size_ = static_cast<std::uint32_t>(yyjson_get_len(value));
        return true;
    case YYJSON_TYPE_ARR: {
        const auto count = yyjson_arr_size(value);
        auto* const elements = storage.allocateArray<JsonValue>(count);
        out.kind_ = JsonValueKind::Array;
        out.data_ = elements;
        out.size_ = static_cast<std::uint32_t>(count);
        yyjson_arr_iter iter{};
        yyjson_arr_iter_init(value, &iter);
        std::size_t index = 0;
        for (yyjson_val* element = yyjson_arr_iter_next(&iter); element != nullptr;
             element = yyjson_arr_iter_next(&iter)) {
            path.pushIndex(index);
            const bool built = buildValue(element, elements[index], path, storage, errorPath);
            path.pop();
            if (!built) {
                return false;
            }
            ++index;
        }
        return true;
    }
    case YYJSON_TYPE_OBJ: {
        const auto count = yyjson_obj_size(value);
        auto* const members = storage.allocateArray<JsonMember>(count);
        out.kind_ = JsonValueKind::Object;
        out.data_ = members;
        out.size_ = static_cast<std::uint32_t>(count);
        // Decoded duplicate keys are refused. A small object -- every object the project format
        // writes -- compares against its earlier keys directly; only a large one pays for a set.
        constexpr std::size_t kLinearDuplicateCheckMembers = 32;
        std::optional<std::pmr::unordered_set<std::string_view>> seenKeys;
        if (count > kLinearDuplicateCheckMembers) {
            seenKeys.emplace(&storage.resource);
            seenKeys->reserve(count);
        }
        yyjson_obj_iter iter{};
        yyjson_obj_iter_init(value, &iter);
        std::size_t index = 0;
        for (yyjson_val* key = yyjson_obj_iter_next(&iter); key != nullptr;
             key = yyjson_obj_iter_next(&iter)) {
            yyjson_val* const memberValue = yyjson_obj_iter_get_val(key);
            const std::string_view decodedKey(yyjson_get_str(key), yyjson_get_len(key));
            const bool duplicate =
                seenKeys.has_value()
                    ? !seenKeys->insert(decodedKey).second
                    : std::any_of(members, members + index, [&](const JsonMember& earlier) {
                          return earlier.key() == decodedKey;
                      });
            if (duplicate) {
                errorPath = formatPath(path, decodedKey);
                return false;
            }
            auto& member = members[index];
            member.key_ = decodedKey.data();
            member.keySize_ = static_cast<std::uint32_t>(decodedKey.size());
            path.pushKey(decodedKey);
            const bool built = buildValue(memberValue, member.value_, path, storage, errorPath);
            path.pop();
            if (!built) {
                return false;
            }
            ++index;
        }
        return true;
    }
    default:
        // YYJSON_TYPE_NONE is unreachable: the preceding preflight scan already proved `input`
        // holds exactly one well-formed root value.
        return true;
    }
}

StrictJsonDomPathText StrictJsonDomBuilder::formatPath(const PathStack& stack,
                                                       const std::string_view finalKey) noexcept {
    StrictJsonDomPathText text;
    const auto appendSlice = [&text](const std::string_view piece) noexcept {
        if (text.truncated_) {
            return;
        }
        const auto available = text.chars_.size() - text.size_;
        const auto copyBytes = std::min(available, piece.size());
        std::memcpy(text.chars_.data() + text.size_, piece.data(), copyBytes);
        text.size_ = static_cast<std::uint16_t>(text.size_ + copyBytes);
        if (copyBytes < piece.size()) {
            text.truncated_ = true;
        }
    };

    for (const auto& segment : stack.segments()) {
        appendSlice("/");
        if (segment.isIndex) {
            std::array<char, 24> buffer{};
            const auto conversion =
                std::to_chars(buffer.data(), buffer.data() + buffer.size(), segment.index);
            appendSlice(std::string_view(buffer.data(),
                                         static_cast<std::size_t>(conversion.ptr - buffer.data())));
        } else {
            appendSlice(segment.key);
        }
    }
    appendSlice("/");
    appendSlice(finalKey);
    return text;
}

StrictJsonDomResult StrictJsonDomBuilder::makeFailure(const StrictJsonDomError error,
                                                      const std::size_t byteOffset) noexcept {
    StrictJsonDomResult result;
    result.error_ = error;
    result.byteOffset_ = byteOffset;
    return result;
}

StrictJsonDomResult StrictJsonDomBuilder::makeSuccess(StrictJsonDomDocument document) noexcept {
    StrictJsonDomResult result;
    result.error_ = StrictJsonDomError::None;
    result.document_.emplace(std::move(document));
    return result;
}

StrictJsonDomResult
StrictJsonDomBuilder::makeDuplicateKeyFailure(StrictJsonDomPathText path) noexcept {
    StrictJsonDomResult result;
    result.error_ = StrictJsonDomError::DuplicateObjectKey;
    result.path_ = path;
    return result;
}

void* StrictJsonDomBuilder::allocatorMalloc(void* const ctx, const std::size_t size) noexcept {
    auto* const resource = static_cast<ProjectIoMemoryResource*>(ctx);
    const auto result = resource->checkedAllocate(size, alignof(std::max_align_t));
    return result ? result.pointer() : nullptr;
}

void* StrictJsonDomBuilder::allocatorRealloc(void* const ctx, void* const ptr,
                                             const std::size_t oldSize,
                                             const std::size_t size) noexcept {
    auto* const resource = static_cast<ProjectIoMemoryResource*>(ctx);
    const auto result = resource->checkedAllocate(size, alignof(std::max_align_t));
    if (!result) {
        return nullptr;
    }
    if (ptr != nullptr && oldSize > 0) {
        std::memcpy(result.pointer(), ptr, std::min(oldSize, size));
    }
    if (ptr != nullptr) {
        resource->deallocate(ptr, oldSize, alignof(std::max_align_t));
    }
    return result.pointer();
}

void StrictJsonDomBuilder::allocatorFree(void* const ctx, void* const ptr) noexcept {
    if (ptr == nullptr) {
        return;
    }
    auto* const resource = static_cast<ProjectIoMemoryResource*>(ctx);
    resource->deallocate(ptr, 0, alignof(std::max_align_t));
}

} // namespace bloom::project::detail

namespace bloom::project {

StrictJsonDomResult parseStrictJsonDom(const std::span<const std::byte> input,
                                       const StrictJsonDomLimits& limits,
                                       ProjectIoOperationMemory operation) noexcept {
    return detail::StrictJsonDomBuilder::parse(input, limits, std::move(operation));
}

} // namespace bloom::project
