// #752: subtitle text encodings, docs/architecture/subtitles.md "Encoding". A byte order mark
// names UTF-8 or UTF-16; without one the text must be UTF-8 unless the caller names an encoding,
// and nothing is guessed. Single-byte encodings are decoded from the WHATWG Encoding Standard's
// index tables (whatwg_single_byte_tables.inc).
#include <bloom/subtitles/subtitles.hpp>

#include <bloom/core/utf8.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace bloom::subtitles {
namespace {

struct WhatwgSingleByteEncoding {
    std::string_view name;
    const std::array<char32_t, 128>* table;
};
struct WhatwgEncodingLabel {
    std::string_view label;
    std::size_t encoding;
};

#include "whatwg_single_byte_tables.inc"

// WHATWG labels of encodings Bloom refuses: the multibyte legacy encodings and UTF-16 without a
// byte order mark's certainty is not among them (utf-16le/be are named explicitly).
constexpr std::array<std::string_view, 22> kRefusedLabels{
    "big5",       "big5-hkscs", "cn-big5",    "csbig5",     "x-x-big5",   "euc-jp",
    "cseucpkdfmtjapanese",      "x-euc-jp",   "iso-2022-jp", "csiso2022jp", "shift_jis",
    "csshiftjis", "ms932",      "ms_kanji",   "sjis",       "windows-31j", "x-sjis",
    "euc-kr",     "gbk",        "gb18030",    "gb2312",     "x-gbk"};

void appendUtf8(std::string& out, const char32_t point) {
    if (point < 0x80) {
        out.push_back(static_cast<char>(point));
    } else if (point < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (point >> 6)));
        out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
    } else if (point < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((point >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
    }
}

[[nodiscard]] std::string normalizedLabel(const std::string_view label) {
    // The WHATWG "get an encoding": strip ASCII whitespace, compare ASCII case-insensitively.
    std::size_t first = 0;
    std::size_t last = label.size();
    const auto space = [](const char c) {
        return c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ';
    };
    while (first < last && space(label[first]))
        ++first;
    while (last > first && space(label[last - 1]))
        --last;
    std::string out(label.substr(first, last - first));
    for (auto& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

[[nodiscard]] std::size_t utf8LineAt(const std::string_view text, const std::size_t offset) {
    return 1 + static_cast<std::size_t>(
                   std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(offset), '\n'));
}

[[nodiscard]] std::variant<DecodedText, DecodeFailure>
decodeUtf8(const std::span<const std::byte> bytes, const std::size_t skip) {
    std::string text(reinterpret_cast<const char*>(bytes.data()) + skip, bytes.size() - skip);
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto scalar = core::decodeUtf8Scalar(text, offset);
        if (!scalar.isValid())
            return DecodeFailure{utf8LineAt(text, offset), offset + skip,
                                 "line " + std::to_string(utf8LineAt(text, offset)) +
                                     ": the byte at offset " + std::to_string(offset + skip) +
                                     " is not UTF-8; name the file's encoding, or save it as "
                                     "UTF-8"};
        offset += scalar.length;
    }
    return DecodedText{std::move(text), "utf-8"};
}

[[nodiscard]] std::variant<DecodedText, DecodeFailure>
decodeUtf16(const std::span<const std::byte> bytes, const std::size_t skip, const bool little) {
    std::string text;
    text.reserve(bytes.size());
    std::size_t line = 1;
    const auto unitAt = [&](const std::size_t index) {
        const auto a = static_cast<std::uint32_t>(bytes[index]);
        const auto b = static_cast<std::uint32_t>(bytes[index + 1]);
        return little ? (a | (b << 8U)) : ((a << 8U) | b);
    };
    const auto failure = [&](const std::size_t offset, const std::string& what) {
        return DecodeFailure{line, offset,
                             "line " + std::to_string(line) + ": " + what + " at byte offset " +
                                 std::to_string(offset) + " is not UTF-16"};
    };
    std::size_t index = skip;
    while (index < bytes.size()) {
        if (index + 1 >= bytes.size())
            return failure(index, "the last byte");
        const auto unit = unitAt(index);
        char32_t point = unit;
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (index + 3 >= bytes.size())
                return failure(index, "an unpaired surrogate");
            const auto low = unitAt(index + 2);
            if (low < 0xDC00 || low > 0xDFFF)
                return failure(index, "an unpaired surrogate");
            point = 0x10000 + ((unit - 0xD800) << 10U) + (low - 0xDC00);
            index += 4;
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            return failure(index, "an unpaired surrogate");
        } else {
            index += 2;
        }
        if (point == '\n')
            ++line;
        appendUtf8(text, point);
    }
    return DecodedText{std::move(text), little ? "utf-16le" : "utf-16be"};
}

} // namespace

std::optional<std::string_view> singleByteEncodingName(const std::string_view label) {
    const auto key = normalizedLabel(label);
    const auto found = std::lower_bound(
        kWhatwgSingleByteLabels.begin(), kWhatwgSingleByteLabels.end(), key,
        [](const WhatwgEncodingLabel& entry, const std::string& wanted) {
            return entry.label < std::string_view(wanted);
        });
    if (found == kWhatwgSingleByteLabels.end() || found->label != key)
        return std::nullopt;
    return kWhatwgSingleByteEncodings[found->encoding].name;
}

std::vector<std::string_view> singleByteEncodingNames() {
    std::vector<std::string_view> names;
    for (const auto& encoding : kWhatwgSingleByteEncodings)
        names.push_back(encoding.name);
    return names;
}

std::variant<DecodedText, DecodeFailure> decodeText(const std::span<const std::byte> bytes,
                                                    const std::string_view encoding) {
    const auto starts = [&](std::initializer_list<unsigned char> prefix) {
        if (bytes.size() < prefix.size())
            return false;
        std::size_t index = 0;
        for (const auto byte : prefix)
            if (bytes[index++] != std::byte{byte})
                return false;
        return true;
    };
    const auto named = normalizedLabel(encoding);
    // A byte order mark decides, whatever was named (the WHATWG "decode" algorithm).
    if (starts({0xEF, 0xBB, 0xBF}))
        return decodeUtf8(bytes, 3);
    if (starts({0xFF, 0xFE}))
        return decodeUtf16(bytes, 2, true);
    if (starts({0xFE, 0xFF}))
        return decodeUtf16(bytes, 2, false);
    if (named.empty() || named == "utf-8" || named == "utf8" || named == "unicode-1-1-utf-8")
        return decodeUtf8(bytes, 0);
    if (named == "utf-16le" || named == "utf-16")
        return decodeUtf16(bytes, 0, true);
    if (named == "utf-16be")
        return decodeUtf16(bytes, 0, false);
    if (std::ranges::find(kRefusedLabels, named) != kRefusedLabels.end())
        return DecodeFailure{0, 0,
                             "the multibyte encoding \"" + named +
                                 "\" is not read; save the file as UTF-8"};
    const auto label = std::lower_bound(
        kWhatwgSingleByteLabels.begin(), kWhatwgSingleByteLabels.end(), named,
        [](const WhatwgEncodingLabel& entry, const std::string& wanted) {
            return entry.label < std::string_view(wanted);
        });
    if (label == kWhatwgSingleByteLabels.end() || label->label != named)
        return DecodeFailure{0, 0,
                             "\"" + named +
                                 "\" is not an encoding Bloom reads: UTF-8, UTF-16LE, UTF-16BE "
                                 "or a WHATWG single-byte encoding such as windows-1252"};
    const auto& table = *kWhatwgSingleByteEncodings[label->encoding].table;
    std::string text;
    text.reserve(bytes.size());
    std::size_t line = 1;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto byte = static_cast<unsigned char>(bytes[index]);
        if (byte < 0x80) {
            if (byte == '\n')
                ++line;
            text.push_back(static_cast<char>(byte));
            continue;
        }
        const auto point = table[byte - 0x80U];
        if (point == 0)
            return DecodeFailure{line, index,
                                 "line " + std::to_string(line) + ": the byte at offset " +
                                     std::to_string(index) + " has no character in " +
                                     std::string(kWhatwgSingleByteEncodings[label->encoding].name)};
        appendUtf8(text, point);
    }
    return DecodedText{std::move(text), std::string(kWhatwgSingleByteEncodings[label->encoding].name)};
}

} // namespace bloom::subtitles
