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

bool clearSubtitleTracksEnabled(std::FILE* file, const std::uint64_t size) {
    if (std::fflush(file) != 0)
        return false;
    const auto moov = findChild(file, 0, size, "moov");
    if (!moov)
        return false;
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
        if (!hdlr || hdlr->size < hdlr->header + 12)
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
    }
    return std::fflush(file) == 0;
}

} // namespace bloom::media::ffmpeg
