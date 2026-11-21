// #752: the subtitle entry points (docs/architecture/subtitles.md, "Formats" and "Limits"):
// format names, sniffing, the size limit, the formats refused by name, file reads with their
// digest and stamp, and write dispatch.
#include "internal.hpp"

#include <bloom/core/sha256.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <system_error>

namespace bloom::subtitles {
namespace {

struct FormatName {
    std::string_view name;
    Format format;
};
constexpr std::array<FormatName, 8> kNames{{{"srt", Format::Srt},
                                            {"subrip", Format::Srt},
                                            {"vtt", Format::WebVtt},
                                            {"webvtt", Format::WebVtt},
                                            {"ttml", Format::Ttml},
                                            {"dfxp", Format::Ttml},
                                            {"imsc", Format::Ttml},
                                            {"ass", Format::Ass}}};

// Formats Bloom refuses, by extension, with the reason.
struct Refused {
    std::string_view extension;
    std::string_view message;
};
constexpr std::array<Refused, 8> kRefused{{
    {".scc", "SCC (CEA-608 caption data) is not read; convert it to SRT, WebVTT or TTML"},
    {".stl", "EBU STL (binary teletext subtitles) is not read; convert it to SRT, WebVTT or TTML"},
    {".sbv", "SBV is not read; convert it to SRT or WebVTT"},
    {".lrc", "LRC lyrics are not read; convert them to SRT or WebVTT"},
    {".sup", "PGS image subtitles are not read; Bloom reads text subtitles"},
    {".idx", "VobSub image subtitles are not read; Bloom reads text subtitles"},
    {".sub", "VobSub and MicroDVD .sub files are not read; convert them to SRT or WebVTT"},
    {".smi", "SAMI is not read; convert it to SRT or WebVTT"},
}};

[[nodiscard]] std::string extensionOf(const std::string_view name) {
    const auto slash = name.find_last_of("/\\");
    const auto base = slash == std::string_view::npos ? name : name.substr(slash + 1);
    const auto dot = base.rfind('.');
    return dot == std::string_view::npos ? std::string{} : detail::lower(base.substr(dot));
}

[[nodiscard]] LoadFailure refusal(const std::string_view code, std::string message) {
    LoadFailure failure;
    failure.code = std::string(code);
    failure.message = std::move(message);
    return failure;
}

[[nodiscard]] bool startsWith(const std::span<const std::byte> bytes, const std::string_view text,
                              const std::size_t at = 0) {
    if (bytes.size() < at + text.size())
        return false;
    for (std::size_t index = 0; index < text.size(); ++index)
        if (static_cast<char>(bytes[at + index]) != text[index])
            return false;
    return true;
}

// Binary formats, recognized from their first bytes before any decoding.
[[nodiscard]] std::optional<std::string_view> refusedBinary(const std::span<const std::byte> bytes) {
    if (startsWith(bytes, "PG"))
        return "PGS image subtitles are not read; Bloom reads text subtitles";
    if (bytes.size() >= 4 && bytes[0] == std::byte{0} && bytes[1] == std::byte{0} &&
        bytes[2] == std::byte{1} && bytes[3] == std::byte{0xBA})
        return "VobSub image subtitles are not read; Bloom reads text subtitles";
    if (startsWith(bytes, "STL", 3))
        return "EBU STL (binary teletext subtitles) is not read; convert it to SRT, WebVTT or "
               "TTML";
    return std::nullopt;
}

// Text formats Bloom refuses, recognized from their first line.
[[nodiscard]] std::optional<std::string_view> refusedText(const std::string_view text) {
    if (text.starts_with("Scenarist_SCC"))
        return "SCC (CEA-608 caption data) is not read; convert it to SRT, WebVTT or TTML";
    if (text.starts_with("# VobSub index file"))
        return "VobSub image subtitles are not read; Bloom reads text subtitles";
    if (detail::lower(text.substr(0, 6)) == "<sami>")
        return "SAMI is not read; convert it to SRT or WebVTT";
    return std::nullopt;
}

[[nodiscard]] bool isTtmlText(const std::string_view text) {
    // An XML document whose root is <tt> (any prefix) in a TTML namespace.
    return text.find("http://www.w3.org/ns/ttml") != std::string_view::npos ||
           text.find("http://www.w3.org/2006/10/ttaf1") != std::string_view::npos ||
           text.find("http://www.w3.org/2006/04/ttaf1") != std::string_view::npos;
}

[[nodiscard]] std::optional<Format> sniff(const std::string_view text) {
    auto start = text;
    while (!start.empty() && (start.front() == ' ' || start.front() == '\t' ||
                              start.front() == '\n' || start.front() == '\r'))
        start.remove_prefix(1);
    if (text.starts_with("WEBVTT"))
        return Format::WebVtt;
    if (start.starts_with("<")) {
        if (isTtmlText(text))
            return Format::Ttml;
        return std::nullopt;
    }
    const auto head = detail::lower(start.substr(0, 64));
    if (head.starts_with("[script info]") || head.starts_with("[v4+ styles]") ||
        head.starts_with("[v4 styles]") || head.starts_with("[events]"))
        return Format::Ass;
    if (text.find("-->") != std::string_view::npos)
        return Format::Srt;
    return std::nullopt;
}

[[nodiscard]] LoadOutcome loadDecoded(const std::string_view text, std::string encoding,
                                      const LoadOptions& options) {
    if (const auto reason = refusedText(text))
        return refusal(codes::kRefusedFormat, std::string(*reason));
    auto format = options.format;
    if (!format)
        format = sniff(text);
    if (!format && !options.sourceName.empty())
        format = formatFromExtension(extensionOf(options.sourceName));
    if (!format)
        return refusal(codes::kUnrecognized,
                       "The text is not a subtitle format Bloom reads (SRT, WebVTT, TTML or "
                       "ASS/SSA); name the format if the file is one of them");
    detail::Builder builder(options);
    switch (*format) {
    case Format::Srt:
        return detail::readSrt(text, builder, std::move(encoding));
    case Format::WebVtt:
        return detail::readWebVtt(text, builder, std::move(encoding));
    case Format::Ttml:
        return detail::readTtml(text, builder, std::move(encoding));
    case Format::Ass:
        return detail::readAss(text, builder, std::move(encoding));
    }
    return refusal(codes::kUnrecognized, "The format is not read.");
}

[[nodiscard]] std::string sizeLimitMessage(const std::uint64_t size) {
    return "The file is " + std::to_string(size) + " bytes; Bloom reads subtitle files of at most " +
           std::to_string(document::kMaximumSubtitleFileBytes) + " bytes (16 MiB)";
}

[[nodiscard]] std::int64_t stampOf(const std::filesystem::file_time_type time) {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count());
}

[[nodiscard]] std::string pathText(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

} // namespace

std::string_view formatName(const Format format) noexcept {
    switch (format) {
    case Format::Srt:
        return "srt";
    case Format::WebVtt:
        return "vtt";
    case Format::Ttml:
        return "ttml";
    case Format::Ass:
        return "ass";
    }
    return "srt";
}

std::optional<Format> formatFromName(const std::string_view name) noexcept {
    for (const auto& entry : kNames)
        if (detail::equalsIgnoringCase(entry.name, name))
            return entry.format;
    if (detail::equalsIgnoringCase(name, "ssa"))
        return Format::Ass;
    return std::nullopt;
}

std::optional<Format> formatFromExtension(std::string_view extension) noexcept {
    if (extension.starts_with("."))
        extension.remove_prefix(1);
    if (detail::equalsIgnoringCase(extension, "srt"))
        return Format::Srt;
    if (detail::equalsIgnoringCase(extension, "vtt"))
        return Format::WebVtt;
    if (detail::equalsIgnoringCase(extension, "ttml") ||
        detail::equalsIgnoringCase(extension, "dfxp") || detail::equalsIgnoringCase(extension, "xml"))
        return Format::Ttml;
    if (detail::equalsIgnoringCase(extension, "ass") || detail::equalsIgnoringCase(extension, "ssa"))
        return Format::Ass;
    return std::nullopt;
}

LoadOutcome load(const std::span<const std::byte> bytes, const LoadOptions& options) {
    if (bytes.size() > document::kMaximumSubtitleFileBytes)
        return refusal(codes::kLimit, sizeLimitMessage(bytes.size()));
    if (options.stop.stop_requested())
        return refusal(codes::kCancelled, "The load was cancelled.");
    if (!options.format) {
        if (const auto reason = refusedBinary(bytes))
            return refusal(codes::kRefusedFormat, std::string(*reason));
        const auto extension = extensionOf(options.sourceName);
        for (const auto& refused : kRefused)
            if (extension == refused.extension)
                return refusal(codes::kRefusedFormat, std::string(refused.message));
    }
    auto decoded = decodeText(bytes, options.encoding);
    if (const auto* failed = std::get_if<DecodeFailure>(&decoded)) {
        LoadFailure failure;
        failure.line = failed->line;
        failure.code = std::string(codes::kEncoding);
        failure.message = failed->message;
        return failure;
    }
    auto& text = std::get<DecodedText>(decoded);
    return loadDecoded(text.text, std::move(text.encoding), options);
}

LoadOutcome load(const std::string_view text, const LoadOptions& options) {
    // Text is Unicode already: read as UTF-8, whatever encoding the options name.
    auto own = options;
    own.encoding = "utf-8";
    return load(std::as_bytes(std::span(text.data(), text.size())), own);
}

std::optional<FileStamp> statFile(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error)
        return std::nullopt;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return std::nullopt;
    const auto modified = std::filesystem::last_write_time(path, error);
    if (error)
        return std::nullopt;
    return FileStamp{static_cast<std::uint64_t>(size), stampOf(modified)};
}

std::string digestOf(const std::span<const std::byte> bytes) {
    const auto digest = core::Sha256Hasher::hash(bytes);
    if (!digest)
        return {};
    const auto hex = digest->toLowercaseHex();
    return "sha256:" + std::string(hex.data(), hex.size());
}

FileReadOutcome readFile(const std::filesystem::path& path, const std::stop_token stop) {
    std::error_code error;
    if (!std::filesystem::exists(path, error))
        return refusal(codes::kFile, "The file " + pathText(path) + " does not exist.");
    const auto stamp = statFile(path);
    if (!stamp)
        return refusal(codes::kFile, "The file " + pathText(path) + " is not a readable file.");
    if (stamp->size > document::kMaximumSubtitleFileBytes)
        return refusal(codes::kLimit, sizeLimitMessage(stamp->size));
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return refusal(codes::kFile, "The file " + pathText(path) + " could not be opened.");
    FileContents contents;
    contents.bytes.reserve(static_cast<std::size_t>(stamp->size));
    constexpr std::size_t kPiece = 1U << 16U;
    std::array<char, kPiece> buffer{};
    core::Sha256Hasher hasher;
    while (stream) {
        if (stop.stop_requested())
            return refusal(codes::kCancelled, "The load was cancelled.");
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(stream.gcount());
        if (count == 0)
            break;
        if (contents.bytes.size() + count > document::kMaximumSubtitleFileBytes)
            return refusal(codes::kLimit, sizeLimitMessage(contents.bytes.size() + count));
        const auto* first = reinterpret_cast<const std::byte*>(buffer.data());
        contents.bytes.insert(contents.bytes.end(), first, first + count);
        static_cast<void>(hasher.update(std::span(first, count)));
    }
    if (stream.bad())
        return refusal(codes::kFile, "The file " + pathText(path) + " could not be read.");
    const auto hex = hasher.finalize().toLowercaseHex();
    contents.digest = "sha256:" + std::string(hex.data(), hex.size());
    contents.size = contents.bytes.size();
    contents.modifiedNanoseconds = stamp->modifiedNanoseconds;
    return contents;
}

std::optional<WriteResult> write(const document::CueListValue& cues, const Format format,
                                 const WriteOptions& options) {
    WriteResult result;
    switch (format) {
    case Format::Srt:
        result.text = detail::writeSrt(cues, result.reports);
        return result;
    case Format::WebVtt:
        result.text = detail::writeWebVtt(cues, result.reports);
        return result;
    case Format::Ttml:
        result.text = detail::writeTtml(cues, options, result.reports);
        return result;
    case Format::Ass:
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace bloom::subtitles
