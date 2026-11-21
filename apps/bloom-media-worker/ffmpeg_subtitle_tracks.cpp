#include "ffmpeg_subtitle_tracks.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <optional>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/mathematics.h>
}

namespace bloom::media::ffmpeg {
namespace {

using provider::Error;
using provider::Unavailable;

[[noreturn]] void refuse(const std::string& detail) { throw Unavailable{Error::Corrupt, detail}; }

// The first frame at `rate` whose time is at or after `time` (in `base` units): the frame a
// player first shows a sample that starts at `time` on, or the frame after a sample's last.
std::uint64_t firstFrameAtOrAfter(const std::int64_t time, const AVRational base,
                                  const provider::Rational rate) {
    if (time <= 0)
        return 0;
    const auto frames =
        av_rescale_rnd(time, static_cast<std::int64_t>(base.num) * rate.numerator,
                       static_cast<std::int64_t>(base.den) * rate.denominator, AV_ROUND_UP);
    return frames < 0 ? 0 : static_cast<std::uint64_t>(frames);
}

std::string metadata(const AVDictionary* dictionary, const char* key) {
    const auto* entry = av_dict_get(dictionary, key, nullptr, 0);
    return entry != nullptr && entry->value != nullptr ? std::string(entry->value) : std::string();
}

// ISO BMFF boxes, read with stdio from the artifact being finished.
bool seekTo(std::FILE* file, const std::uint64_t at) {
#if defined(_WIN32)
    return _fseeki64(file, static_cast<__int64>(at), SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(at), SEEK_SET) == 0;
#endif
}

struct Box final {
    std::uint64_t offset = 0, size = 0, header = 0;
    std::array<char, 4> type{};
    [[nodiscard]] bool is(const char* name) const { return std::memcmp(type.data(), name, 4) == 0; }
    [[nodiscard]] std::uint64_t contentStart() const { return offset + header; }
    [[nodiscard]] std::uint64_t end() const { return offset + size; }
};

std::optional<Box> readBox(std::FILE* file, const std::uint64_t at, const std::uint64_t limit) {
    std::array<unsigned char, 16> head{};
    if (limit < at || limit - at < 8 || !seekTo(file, at) ||
        std::fread(head.data(), 1, 8, file) != 8)
        return std::nullopt;
    Box box;
    box.offset = at;
    box.header = 8;
    std::uint64_t size = 0;
    for (std::size_t index = 0; index < 4; ++index)
        size = (size << 8U) | head[index];
    std::memcpy(box.type.data(), head.data() + 4, 4);
    if (size == 1) {
        if (limit - at < 16 || std::fread(head.data() + 8, 1, 8, file) != 8)
            return std::nullopt;
        size = 0;
        for (std::size_t index = 8; index < 16; ++index)
            size = (size << 8U) | head[index];
        box.header = 16;
    } else if (size == 0) {
        size = limit - at;
    }
    if (size < box.header || size > limit - at)
        return std::nullopt;
    box.size = size;
    return box;
}

// The child of [start, end) named `name`.
std::optional<Box> findChild(std::FILE* file, std::uint64_t start, const std::uint64_t end,
                             const char* name) {
    while (start < end) {
        const auto box = readBox(file, start, end);
        if (!box)
            return std::nullopt;
        if (box->is(name))
            return box;
        start = box->end();
    }
    return std::nullopt;
}

} // namespace

std::string assDialogueText(const std::string_view text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char c = text[index];
        if (c == '\n') {
            out += "\\N";
            continue;
        }
        out.push_back(c);
        const char next = index + 1 < text.size() ? text[index + 1] : '\0';
        // ASS reads "\n" and "\N" as line breaks and "{\" as the start of an override block; an
        // empty reset block between the two characters keeps them as written.
        if ((c == '\\' && (next == 'n' || next == 'N')) || (c == '{' && next == '\\'))
            out += "{\\r}";
    }
    return out;
}

std::string assScriptHeader(const std::uint32_t width, const std::uint32_t height) {
    return "[Script Info]\r\nScriptType: v4.00+\r\nPlayResX: " + std::to_string(width) +
           "\r\nPlayResY: " + std::to_string(height) +
           "\r\nScaledBorderAndShadow: yes\r\n\r\n[V4+ Styles]\r\n"
           "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, "
           "BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, "
           "BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\r\n"
           "Style: Default,Sans-Serif," +
           std::to_string(std::max<std::uint32_t>(16U, height / 18U)) +
           ",&Hffffff,&Hffffff,&H0,&H0,0,0,0,0,100,100,0,0,1,1,0,2,10,10,10,0\r\n\r\n"
           "[Events]\r\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, "
           "Effect, Text\r\n";
}

std::int64_t subtitleTrackTime(const std::uint64_t frame, const provider::Rational rate,
                               const bool milliseconds) {
    if (!milliseconds)
        return static_cast<std::int64_t>(frame);
    return av_rescale_rnd(static_cast<std::int64_t>(frame), 1000 * rate.denominator, rate.numerator,
                          AV_ROUND_DOWN);
}

std::vector<provider::SubtitleTrackV1> readSubtitleTracks(AVFormatContext& opened,
                                                          const provider::Rational rate) {
    std::vector<int> streams;
    std::vector<provider::SubtitleTrackV1> tracks;
    for (unsigned index = 0; index < opened.nb_streams; ++index) {
        const auto* stream = opened.streams[index];
        if (stream->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE)
            continue;
        if (stream->codecpar->codec_id != AV_CODEC_ID_MOV_TEXT &&
            stream->codecpar->codec_id != AV_CODEC_ID_WEBVTT)
            refuse("Reopened subtitle track " + std::to_string(tracks.size() + 1) +
                   " is not 3GPP Timed Text or WebVTT");
        if ((stream->disposition & (AV_DISPOSITION_DEFAULT | AV_DISPOSITION_FORCED)) != 0)
            refuse("Reopened subtitle track " + std::to_string(tracks.size() + 1) +
                   " is marked as the default or a forced track");
        provider::SubtitleTrackV1 track;
        track.language = metadata(stream->metadata, "language");
        if (track.language.empty())
            track.language = "und";
        // Matroska names the track "title"; FFmpeg's MOV demuxer reads a track's user data name
        // (udta/name, which the MOV muxer and AVAssetWriter write) as "name".
        track.title = metadata(stream->metadata, "title");
        if (track.title.empty())
            track.title = metadata(stream->metadata, "name");
        tracks.push_back(std::move(track));
        streams.push_back(static_cast<int>(index));
    }
    if (tracks.empty())
        return tracks;
    std::unique_ptr<AVPacket, void (*)(AVPacket*)> packet(
        av_packet_alloc(), [](AVPacket* value) { av_packet_free(&value); });
    if (!packet)
        throw Unavailable{Error::Oversized, "Reopen allocation"};
    int status = 0;
    while ((status = av_read_frame(&opened, packet.get())) >= 0) {
        const auto found = std::ranges::find(streams, packet->stream_index);
        if (found == streams.end()) {
            av_packet_unref(packet.get());
            continue;
        }
        const auto track = static_cast<std::size_t>(found - streams.begin());
        const auto* stream = opened.streams[packet->stream_index];
        const auto* data = packet->data;
        const auto size = static_cast<std::size_t>(packet->size);
        std::string text;
        if (stream->codecpar->codec_id == AV_CODEC_ID_MOV_TEXT) {
            // A 3GPP Timed Text sample: a 16-bit big-endian length, the text, then style boxes.
            if (size < 2)
                refuse("Reopened subtitle track " + std::to_string(track + 1) +
                       " holds a truncated sample");
            const auto length = static_cast<std::size_t>((data[0] << 8U) | data[1]);
            if (length > size - 2)
                refuse("Reopened subtitle track " + std::to_string(track + 1) +
                       " holds a truncated sample");
            text.assign(reinterpret_cast<const char*>(data) + 2, length);
        } else {
            // A WebVTT block holds the cue payload.
            text.assign(reinterpret_cast<const char*>(data), size);
        }
        const auto start = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
        const auto duration = packet->duration;
        av_packet_unref(packet.get());
        if (text.empty())
            continue; // the gap between two cues
        if (start == AV_NOPTS_VALUE || duration <= 0)
            refuse("Reopened subtitle track " + std::to_string(track + 1) +
                   " holds a sample without a time");
        provider::SubtitleCueV1 cue;
        cue.first = firstFrameAtOrAfter(start, stream->time_base, rate);
        cue.end = firstFrameAtOrAfter(start + duration, stream->time_base, rate);
        cue.text = std::move(text);
        tracks[track].cues.push_back(std::move(cue));
    }
    if (status != AVERROR_EOF)
        refuse("Reopened subtitle tracks could not be read");
    return tracks;
}

std::vector<provider::SubtitleTrackV1> readSubtitleTracks(const std::string& path,
                                                          const provider::Rational rate) {
    AVFormatContext* input = nullptr;
    if (avformat_open_input(&input, path.c_str(), nullptr, nullptr) < 0)
        refuse("Reopen could not read the subtitle tracks");
    std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> owned(
        input, [](AVFormatContext* value) { avformat_close_input(&value); });
    if (avformat_find_stream_info(input, nullptr) < 0)
        refuse("Reopen could not read the subtitle tracks");
    return readSubtitleTracks(*input, rate);
}

bool finishSubtitleTrackHeaders(std::FILE* file, const std::uint64_t size,
                                const std::vector<std::string>& languages) {
    if (std::fflush(file) != 0)
        return false;
    const auto moov = findChild(file, 0, size, "moov");
    if (!moov)
        return false;
    std::size_t textTrack = 0;
    std::uint64_t at = moov->contentStart();
    while (at < moov->end()) {
        const auto trak = readBox(file, at, moov->end());
        if (!trak)
            return false;
        at = trak->end();
        if (!trak->is("trak"))
            continue;
        const auto tkhd = findChild(file, trak->contentStart(), trak->end(), "tkhd");
        const auto mdia = findChild(file, trak->contentStart(), trak->end(), "mdia");
        if (!tkhd || !mdia)
            return false;
        const auto hdlr = findChild(file, mdia->contentStart(), mdia->end(), "hdlr");
        const auto mdhd = findChild(file, mdia->contentStart(), mdia->end(), "mdhd");
        if (!hdlr || !mdhd || hdlr->size < hdlr->header + 12)
            return false;
        // hdlr: version and flags (4), pre_defined (4), handler_type (4).
        std::array<char, 4> handler{};
        if (!seekTo(file, hdlr->contentStart() + 8) ||
            std::fread(handler.data(), 1, handler.size(), file) != handler.size())
            return false;
        const bool text = std::memcmp(handler.data(), "sbtl", 4) == 0 ||
                          std::memcmp(handler.data(), "text", 4) == 0 ||
                          std::memcmp(handler.data(), "subt", 4) == 0;
        if (!text)
            continue;
        if (textTrack >= languages.size() || languages[textTrack].size() != 3)
            return false;
        const auto& language = languages[textTrack++];
        // tkhd: version (1) and 24-bit flags; bit 0 of the last flag byte is track_enabled.
        if (tkhd->size < tkhd->header + 4)
            return false;
        const auto flagAt = tkhd->contentStart() + 3;
        unsigned char flags = 0;
        if (!seekTo(file, flagAt) || std::fread(&flags, 1, 1, file) != 1)
            return false;
        flags = static_cast<unsigned char>(flags & ~1U);
        if (!seekTo(file, flagAt) || std::fwrite(&flags, 1, 1, file) != 1)
            return false;
        // mdhd: version (1), flags (3), creation and modification times, timescale and duration
        // (4 bytes each in version 0, 8, 8, 4 and 8 in version 1), then the 16-bit language.
        // FFmpeg's MOV muxer writes only the old Macintosh language codes there (an unlisted
        // language becomes "unspecified"); the ISO 639-2/T code packed in three 5-bit letters is
        // what QuickTime and ISO BMFF readers take for any language.
        unsigned char version = 0;
        if (!seekTo(file, mdhd->contentStart()) || std::fread(&version, 1, 1, file) != 1 ||
            version > 1)
            return false;
        const std::uint64_t languageAt = mdhd->contentStart() + (version == 1 ? 32U : 20U);
        if (languageAt + 2 > mdhd->end())
            return false;
        unsigned code = 0;
        for (const char letter : language) {
            if (letter < 'a' || letter > 'z')
                return false;
            code = (code << 5U) | static_cast<unsigned>(letter - 0x60);
        }
        const std::array<unsigned char, 2> packed{static_cast<unsigned char>((code >> 8U) & 0x7fU),
                                                  static_cast<unsigned char>(code & 0xffU)};
        if (!seekTo(file, languageAt) ||
            std::fwrite(packed.data(), 1, packed.size(), file) != packed.size())
            return false;
    }
    return textTrack == languages.size() && std::fflush(file) == 0;
}

namespace {

// An ISO BMFF box inside a buffer: 32-bit sizes only (the boxes edited here are small).
struct Span final {
    std::size_t offset = 0, size = 0;
    std::array<char, 4> type{};
    [[nodiscard]] bool is(const char* name) const { return std::memcmp(type.data(), name, 4) == 0; }
    [[nodiscard]] std::size_t end() const { return offset + size; }
};

std::uint32_t be32(const std::vector<unsigned char>& bytes, const std::size_t at) {
    return (static_cast<std::uint32_t>(bytes[at]) << 24U) |
           (static_cast<std::uint32_t>(bytes[at + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[at + 2]) << 8U) | bytes[at + 3];
}
void putBe32(std::vector<unsigned char>& bytes, const std::size_t at, const std::uint64_t value) {
    for (std::size_t index = 0; index < 4; ++index)
        bytes[at + index] = static_cast<unsigned char>((value >> (24U - 8U * index)) & 0xffU);
}

// The child boxes of [begin, end), or nothing when one is malformed or 64-bit.
std::optional<std::vector<Span>> children(const std::vector<unsigned char>& bytes,
                                          std::size_t begin, const std::size_t end) {
    std::vector<Span> out;
    while (begin < end) {
        if (end - begin < 8)
            return std::nullopt;
        Span span;
        span.offset = begin;
        span.size = be32(bytes, begin);
        std::memcpy(span.type.data(), bytes.data() + begin + 4, 4);
        if (span.size < 8 || span.size > end - begin)
            return std::nullopt;
        out.push_back(span);
        begin = span.end();
    }
    return out;
}

const Span* child(const std::vector<Span>& spans, const char* type) {
    const auto found = std::ranges::find_if(spans, [&](const Span& span) { return span.is(type); });
    return found == spans.end() ? nullptr : &*found;
}

} // namespace

std::optional<std::uint64_t> nameSubtitleTracks(std::FILE* file, const std::uint64_t size,
                                                const std::vector<std::string>& titles) {
    if (std::fflush(file) != 0)
        return std::nullopt;
    const auto moov = findChild(file, 0, size, "moov");
    if (!moov || moov->end() != size || moov->header != 8 || moov->size > (1U << 30U))
        return std::nullopt;
    std::vector<unsigned char> bytes(static_cast<std::size_t>(moov->size));
    if (!seekTo(file, moov->offset) ||
        std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size())
        return std::nullopt;
    const auto tracks = children(bytes, 8, bytes.size());
    if (!tracks)
        return std::nullopt;
    std::vector<unsigned char> out(bytes.begin(), bytes.begin() + 8);
    std::size_t textTrack = 0;
    for (const auto& trak : *tracks) {
        const auto copy = [&](const std::size_t from, const std::size_t to) {
            out.insert(out.end(), bytes.begin() + static_cast<std::ptrdiff_t>(from),
                       bytes.begin() + static_cast<std::ptrdiff_t>(to));
        };
        if (!trak.is("trak")) {
            copy(trak.offset, trak.end());
            continue;
        }
        const auto inside = children(bytes, trak.offset + 8, trak.end());
        const auto* mdia = inside ? child(*inside, "mdia") : nullptr;
        const auto media = mdia ? children(bytes, mdia->offset + 8, mdia->end()) : std::nullopt;
        const auto* hdlr = media ? child(*media, "hdlr") : nullptr;
        if (hdlr == nullptr || hdlr->size < 8 + 12)
            return std::nullopt;
        const auto* handler = bytes.data() + hdlr->offset + 8 + 8;
        const bool text = std::memcmp(handler, "sbtl", 4) == 0 ||
                          std::memcmp(handler, "text", 4) == 0 ||
                          std::memcmp(handler, "subt", 4) == 0;
        if (!text) {
            copy(trak.offset, trak.end());
            continue;
        }
        if (textTrack >= titles.size())
            return std::nullopt;
        const auto& title = titles[textTrack++];
        const auto* udta = child(*inside, "udta");
        const auto data = udta ? children(bytes, udta->offset + 8, udta->end()) : std::nullopt;
        if (udta != nullptr && !data)
            return std::nullopt;
        if (title.empty() || (data && child(*data, "name") != nullptr)) {
            copy(trak.offset, trak.end());
            continue;
        }
        // name: size, type and the UTF-8 title, as FFmpeg writes a track's title.
        std::vector<unsigned char> name(8);
        putBe32(name, 0, 8 + title.size());
        std::memcpy(name.data() + 4, "name", 4);
        name.insert(name.end(), title.begin(), title.end());
        const auto start = out.size();
        if (udta != nullptr) {
            copy(trak.offset, udta->end());
            putBe32(out, start + (udta->offset - trak.offset), udta->size + name.size());
            out.insert(out.end(), name.begin(), name.end());
            copy(udta->end(), trak.end());
        } else {
            copy(trak.offset, trak.end());
            std::vector<unsigned char> box(8);
            putBe32(box, 0, 8 + name.size());
            std::memcpy(box.data() + 4, "udta", 4);
            out.insert(out.end(), box.begin(), box.end());
            out.insert(out.end(), name.begin(), name.end());
        }
        putBe32(out, start, out.size() - start);
    }
    if (textTrack != titles.size() || out.size() > 0xffffffffU)
        return std::nullopt;
    putBe32(out, 0, out.size());
    if (!seekTo(file, moov->offset) || std::fwrite(out.data(), 1, out.size(), file) != out.size() ||
        std::fflush(file) != 0)
        return std::nullopt;
    return moov->offset + out.size();
}

} // namespace bloom::media::ffmpeg
