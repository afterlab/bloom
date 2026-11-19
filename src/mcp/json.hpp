#pragma once

#include <bloom/mcp/protocol.hpp>

#include <yyjson.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace bloom::mcp {

struct InvalidInput final : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// #745, #741: a request that exceeds one of Bloom's advertised bounds. The message names the
// limit, its value and the actual count; the transport answers it with the limit's own JSON-RPC
// code and data {code, limit, value, actual, unit} (describeLimit).
struct LimitExceeded final : std::runtime_error {
    LimitExceeded(McpLimit which, std::uint64_t limitValue, std::uint64_t actualCount,
                  const std::string& message)
        : std::runtime_error(message), limit(which), value(limitValue), actual(actualCount) {}
    McpLimit limit;
    std::uint64_t value;
    std::uint64_t actual;
};

inline constexpr std::size_t kRequestLimit = kMcpMessageLimits.requestBytes;

// The memory one parse or one build may take (McpMessageLimits::arenaBytes, 32 MiB). It is a cap,
// not an allocation: a document allocates only what it holds, so a small request costs what it
// reads (#745; it used to zero-fill a fresh 32 MiB arena per parse and per build).
class JsonBudget final {
  public:
    explicit JsonBudget(std::size_t cap = kMcpMessageLimits.arenaBytes) noexcept;
    JsonBudget(const JsonBudget&) = delete;
    JsonBudget& operator=(const JsonBudget&) = delete;
    [[nodiscard]] yyjson_alc* allocator() noexcept { return &allocator_; }
    [[nodiscard]] std::size_t cap() const noexcept { return cap_; }
    // Bytes held now, and the size of the request that did not fit (0 while none failed).
    [[nodiscard]] std::size_t used() const noexcept { return used_; }
    [[nodiscard]] std::size_t refusedTotal() const noexcept { return refusedTotal_; }
    // Throws LimitExceeded(JsonMemory) when an allocation was refused by the cap.
    void throwIfExceeded() const;

  private:
    static void* allocate(void* context, std::size_t size);
    static void* reallocate(void* context, void* pointer, std::size_t oldSize, std::size_t size);
    static void release(void* context, void* pointer);
    yyjson_alc allocator_{};
    std::size_t cap_ = 0;
    std::size_t used_ = 0;
    std::size_t refusedTotal_ = 0;
};

class Json final {
  public:
    explicit Json(std::size_t memoryCap = kMcpMessageLimits.arenaBytes);
    Json(const Json&) = delete;
    Json& operator=(const Json&) = delete;
    [[nodiscard]] yyjson_mut_val* object();
    [[nodiscard]] yyjson_mut_val* array();
    [[nodiscard]] yyjson_mut_val* text(std::string_view value);
    [[nodiscard]] yyjson_mut_val* number(std::uint64_t value);
    [[nodiscard]] yyjson_mut_val* signedNumber(std::int64_t value);
    [[nodiscard]] yyjson_mut_val* real(double value);
    [[nodiscard]] yyjson_mut_val* boolean(bool value);
    [[nodiscard]] yyjson_mut_val* null();
    [[nodiscard]] yyjson_mut_val* copy(yyjson_val* value);
    void set(yyjson_mut_val* object, std::string_view key, yyjson_mut_val* value);
    void append(yyjson_mut_val* array, yyjson_mut_val* value);
    // Refused with LimitExceeded(ResponseBytes) above `limit` bytes (the response bound).
    [[nodiscard]] std::string encode(yyjson_mut_val* root,
                                     std::size_t limit = kMcpMessageLimits.responseBytes);
    [[nodiscard]] std::size_t bytesUsed() const noexcept { return budget_.used(); }

  private:
    yyjson_mut_val* checked(yyjson_mut_val* value) const;
    JsonBudget budget_;
    std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)> document_;
};

// A parsed JSON document under the message limits: at most `jsonValues` values (array items and
// member values; member names are not counted) and `jsonDepth` levels below the root, no
// duplicate member names, within the memory cap.
class Parsed final {
  public:
    // Throws InvalidInput for malformed JSON or a duplicate member, and LimitExceeded for a
    // value, depth, size or memory bound.
    explicit Parsed(std::string_view source);
    // `deferLimits`: a value or depth bound is recorded in refusal() instead of thrown, so the
    // caller can still read the request's id to answer it.
    Parsed(std::string_view source, bool deferLimits);
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;
    [[nodiscard]] yyjson_val* root() const noexcept { return yyjson_doc_get_root(document_.get()); }
    [[nodiscard]] const std::optional<LimitExceeded>& refusal() const noexcept { return refusal_; }
    // Values counted as above, and the deepest level reached (the root is level 0).
    [[nodiscard]] std::size_t values() const noexcept { return values_; }
    [[nodiscard]] std::size_t depth() const noexcept { return depth_; }
    // The memory the document holds (a fraction of the cap for an ordinary request).
    [[nodiscard]] std::size_t bytesUsed() const noexcept { return budget_.used(); }

  private:
    JsonBudget budget_;
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> document_;
    std::optional<LimitExceeded> refusal_;
    std::size_t values_ = 0;
    std::size_t depth_ = 0;
};

// The size, value count and depth of a document, counted the way Parsed counts them; throws
// InvalidInput on a duplicate member name. Iterative: any depth is measured without recursion.
struct JsonShape final {
    std::size_t values = 0;
    std::size_t depth = 0;
};
[[nodiscard]] JsonShape measure(yyjson_val* root);

// "65,536": a count as the refusals print it.
[[nodiscard]] std::string grouped(std::uint64_t value);

void members(yyjson_val* object, std::initializer_list<std::string_view> allowed,
             std::initializer_list<std::string_view> required = {});
[[nodiscard]] yyjson_val* member(yyjson_val* object, const char* name);
[[nodiscard]] std::string string(yyjson_val* value, std::size_t limit = 4096);
[[nodiscard]] std::uint64_t integer(yyjson_val* value);
// An id: a nonnegative integer, or the same value as a decimal string. Bloom reports ids as strings
// because some (transient export job and output ids) exceed 2^53 and do not survive a double parse.
[[nodiscard]] std::uint64_t identifier(yyjson_val* value);
[[nodiscard]] bool boolean(yyjson_val* value);

} // namespace bloom::mcp
