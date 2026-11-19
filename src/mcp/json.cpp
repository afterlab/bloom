#include "json.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <utility>
#include <vector>

namespace bloom::mcp {
namespace {

// Each allocation carries its size in front, so the budget is exact on free.
constexpr std::size_t kHeader = alignof(std::max_align_t) > sizeof(std::size_t)
                                    ? alignof(std::max_align_t)
                                    : sizeof(std::size_t);

std::size_t& sizeOf(void* block) noexcept { return *static_cast<std::size_t*>(block); }

} // namespace

std::string grouped(const std::uint64_t value) {
    auto digits = std::to_string(value);
    for (auto index = digits.size(); index > 3; index -= 3)
        digits.insert(index - 3, ",");
    return digits;
}

JsonBudget::JsonBudget(const std::size_t cap) noexcept : cap_(cap) {
    allocator_.malloc = &JsonBudget::allocate;
    allocator_.realloc = &JsonBudget::reallocate;
    allocator_.free = &JsonBudget::release;
    allocator_.ctx = this;
}

void* JsonBudget::allocate(void* context, const std::size_t size) {
    auto& budget = *static_cast<JsonBudget*>(context);
    if (size > budget.cap_ - std::min(budget.used_, budget.cap_)) {
        budget.refusedTotal_ = std::max(budget.refusedTotal_, budget.used_ + size);
        return nullptr;
    }
    void* block = std::malloc(kHeader + size);
    if (block == nullptr)
        return nullptr;
    sizeOf(block) = size;
    budget.used_ += size;
    return static_cast<char*>(block) + kHeader;
}

void* JsonBudget::reallocate(void* context, void* pointer, const std::size_t oldSize,
                             const std::size_t size) {
    if (pointer == nullptr)
        return allocate(context, size);
    auto& budget = *static_cast<JsonBudget*>(context);
    void* block = static_cast<char*>(pointer) - kHeader;
    const auto held = sizeOf(block);
    static_cast<void>(oldSize);
    if (size > held && size - held > budget.cap_ - std::min(budget.used_, budget.cap_)) {
        budget.refusedTotal_ = std::max(budget.refusedTotal_, budget.used_ + (size - held));
        return nullptr;
    }
    void* grown = std::realloc(block, kHeader + size);
    if (grown == nullptr)
        return nullptr;
    sizeOf(grown) = size;
    budget.used_ = budget.used_ - held + size;
    return static_cast<char*>(grown) + kHeader;
}

void JsonBudget::release(void* context, void* pointer) {
    if (pointer == nullptr)
        return;
    auto& budget = *static_cast<JsonBudget*>(context);
    void* block = static_cast<char*>(pointer) - kHeader;
    budget.used_ -= sizeOf(block);
    std::free(block);
}

void JsonBudget::throwIfExceeded() const {
    if (refusedTotal_ == 0)
        return;
    throw LimitExceeded(McpLimit::JsonMemory, cap_, refusedTotal_,
                        "The JSON needs at least " + grouped(refusedTotal_) +
                            " bytes of memory; Bloom gives one request or response at most " +
                            grouped(cap_) + " (limit jsonMemoryBytes). " +
                            std::string(kMcpLimitAdvice));
}

JsonShape measure(yyjson_val* root) {
    JsonShape shape;
    if (root == nullptr)
        return shape;
    std::vector<std::pair<yyjson_val*, std::size_t>> pending{{root, 0}};
    std::vector<std::string_view> names;
    while (!pending.empty()) {
        const auto [value, depth] = pending.back();
        pending.pop_back();
        ++shape.values;
        shape.depth = std::max(shape.depth, depth);
        if (yyjson_is_arr(value)) {
            yyjson_arr_iter iterator = yyjson_arr_iter_with(value);
            while (auto* child = yyjson_arr_iter_next(&iterator))
                pending.emplace_back(child, depth + 1);
        } else if (yyjson_is_obj(value)) {
            names.clear();
            yyjson_obj_iter iterator = yyjson_obj_iter_with(value);
            while (auto* key = yyjson_obj_iter_next(&iterator)) {
                names.emplace_back(yyjson_get_str(key), yyjson_get_len(key));
                pending.emplace_back(yyjson_obj_iter_get_val(key), depth + 1);
            }
            std::ranges::sort(names);
            if (std::ranges::adjacent_find(names) != names.end())
                throw InvalidInput("Duplicate decoded object key");
        }
    }
    return shape;
}

yyjson_mut_val* Json::checked(yyjson_mut_val* value) const {
    if (!value) {
        budget_.throwIfExceeded();
        throw std::bad_alloc();
    }
    return value;
}

Json::Json(const std::size_t memoryCap)
    : budget_(memoryCap), document_(nullptr, yyjson_mut_doc_free) {
    document_.reset(yyjson_mut_doc_new(budget_.allocator()));
    if (!document_) {
        budget_.throwIfExceeded();
        throw std::bad_alloc();
    }
}
yyjson_mut_val* Json::object() { return checked(yyjson_mut_obj(document_.get())); }
yyjson_mut_val* Json::array() { return checked(yyjson_mut_arr(document_.get())); }
yyjson_mut_val* Json::text(const std::string_view value) {
    return checked(yyjson_mut_strncpy(document_.get(), value.data(), value.size()));
}
yyjson_mut_val* Json::number(const std::uint64_t value) {
    return checked(yyjson_mut_uint(document_.get(), value));
}
yyjson_mut_val* Json::signedNumber(const std::int64_t value) {
    return checked(yyjson_mut_sint(document_.get(), value));
}
yyjson_mut_val* Json::real(const double value) {
    return checked(yyjson_mut_real(document_.get(), value));
}
yyjson_mut_val* Json::boolean(const bool value) {
    return checked(yyjson_mut_bool(document_.get(), value));
}
yyjson_mut_val* Json::null() { return checked(yyjson_mut_null(document_.get())); }
yyjson_mut_val* Json::copy(yyjson_val* value) {
    return value ? checked(yyjson_val_mut_copy(document_.get(), value)) : null();
}
void Json::set(yyjson_mut_val* object, const std::string_view key, yyjson_mut_val* value) {
    if (!yyjson_mut_obj_add(object, text(key), checked(value))) {
        throw std::bad_alloc();
    }
}
void Json::append(yyjson_mut_val* array, yyjson_mut_val* value) {
    if (!yyjson_mut_arr_append(array, checked(value))) {
        throw std::bad_alloc();
    }
}
std::string Json::encode(yyjson_mut_val* root, const std::size_t limit) {
    yyjson_mut_doc_set_root(document_.get(), root);
    std::size_t size = 0;
    char* encoded = yyjson_mut_write_opts(document_.get(), 0, budget_.allocator(), &size, nullptr);
    if (!encoded) {
        budget_.throwIfExceeded();
        throw std::bad_alloc();
    }
    if (size > limit) {
        budget_.allocator()->free(budget_.allocator()->ctx, encoded);
        throw LimitExceeded(McpLimit::ResponseBytes, limit, size,
                            "The response would be " + grouped(size) +
                                " bytes; Bloom answers with at most " + grouped(limit) +
                                " (limit responseBytes). Ask for less: page the query, name "
                                "fields, use transact's response \"summary\", and send long "
                                "key columns with bloom.animation.set-key-columns and fromFile "
                                "rather than reading them back whole.");
    }
    std::string result(encoded, size);
    budget_.allocator()->free(budget_.allocator()->ctx, encoded);
    return result;
}

Parsed::Parsed(const std::string_view source) : Parsed(source, false) {}

Parsed::Parsed(const std::string_view source, const bool deferLimits)
    : document_(nullptr, yyjson_doc_free) {
    if (source.empty())
        throw InvalidInput("The request is empty");
    if (source.size() > kMcpMessageLimits.requestBytes)
        throw LimitExceeded(McpLimit::RequestBytes, kMcpMessageLimits.requestBytes, source.size(),
                            "The request is " + grouped(source.size()) +
                                " bytes; Bloom reads at most " +
                                grouped(kMcpMessageLimits.requestBytes) +
                                " (limit requestBytes). " + std::string(kMcpLimitAdvice));
    yyjson_read_err error{};
    document_.reset(yyjson_read_opts(const_cast<char*>(source.data()), source.size(),
                                     YYJSON_READ_NOFLAG, budget_.allocator(), &error));
    if (!document_) {
        if (error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION)
            budget_.throwIfExceeded();
        throw InvalidInput(error.msg ? error.msg : "Invalid JSON");
    }
    const auto shape = measure(root());
    // Counted as before #745: the root and every array item and member value.
    values_ = shape.values;
    depth_ = shape.depth;
    const auto& limits = kMcpMessageLimits;
    if (values_ > limits.jsonValues)
        refusal_.emplace(McpLimit::JsonValues, limits.jsonValues, values_,
                         "The request holds " + grouped(values_) +
                             " JSON values; Bloom reads at most " + grouped(limits.jsonValues) +
                             " per request (limit jsonValues). " + std::string(kMcpLimitAdvice));
    else if (depth_ > limits.jsonDepth)
        refusal_.emplace(McpLimit::JsonDepth, limits.jsonDepth, depth_,
                         "The request nests " + grouped(depth_) +
                             " levels deep; Bloom reads at most " + grouped(limits.jsonDepth) +
                             " (limit jsonDepth). " + std::string(kMcpLimitAdvice));
    if (refusal_ && !deferLimits)
        throw *refusal_;
}

void members(yyjson_val* object, const std::initializer_list<std::string_view> allowed,
             const std::initializer_list<std::string_view> required) {
    if (!yyjson_is_obj(object)) {
        throw InvalidInput("Expected an object");
    }
    yyjson_obj_iter iterator = yyjson_obj_iter_with(object);
    while (auto* key = yyjson_obj_iter_next(&iterator)) {
        const std::string_view name(yyjson_get_str(key), yyjson_get_len(key));
        if (std::ranges::find(allowed, name) == allowed.end()) {
            throw InvalidInput("Unknown member: " + std::string(name));
        }
    }
    for (const auto name : required) {
        if (!yyjson_obj_getn(object, name.data(), name.size())) {
            throw InvalidInput("Missing member: " + std::string(name));
        }
    }
}
yyjson_val* member(yyjson_val* object, const char* name) { return yyjson_obj_get(object, name); }
std::string string(yyjson_val* value, const std::size_t limit) {
    if (!yyjson_is_str(value) || yyjson_get_len(value) > limit) {
        throw InvalidInput("Expected a bounded string");
    }
    std::string result(yyjson_get_str(value), yyjson_get_len(value));
    if (result.find('\0') != std::string::npos) {
        throw InvalidInput("NUL is not allowed in strings");
    }
    return result;
}
std::uint64_t integer(yyjson_val* value) {
    if (!yyjson_is_uint(value)) {
        throw InvalidInput("Expected a nonnegative integer");
    }
    return yyjson_get_uint(value);
}
std::uint64_t identifier(yyjson_val* value) {
    if (yyjson_is_str(value)) {
        const std::string_view text(yyjson_get_str(value), yyjson_get_len(value));
        std::uint64_t parsed = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (text.empty() || error != std::errc{} || end != text.data() + text.size())
            throw InvalidInput("Expected an id as a nonnegative integer or decimal string");
        return parsed;
    }
    if (!yyjson_is_uint(value))
        throw InvalidInput("Expected an id as a nonnegative integer or decimal string");
    return yyjson_get_uint(value);
}
bool boolean(yyjson_val* value) {
    if (!yyjson_is_bool(value)) {
        throw InvalidInput("Expected a boolean");
    }
    return yyjson_get_bool(value);
}
} // namespace bloom::mcp
