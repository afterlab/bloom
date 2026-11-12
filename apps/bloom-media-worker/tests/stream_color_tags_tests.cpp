// Issue #667. The reopen check of an H.264 export compares what the stream itself says about its
// color with what the container says. These are real movies, encoded and muxed here with the
// bundled encoder, whose stream and container agree and disagree in each tag.
#include "ffmpeg_stream_color_tags.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
}

namespace {
int failures = 0;
void expect(const bool condition, const std::string_view what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << what << '\n';
    }
}

struct Tags final {
    AVColorPrimaries primaries = AVCOL_PRI_UNSPECIFIED;
    AVColorTransferCharacteristic transfer = AVCOL_TRC_UNSPECIFIED;
    AVColorSpace matrix = AVCOL_SPC_UNSPECIFIED;
    AVColorRange range = AVCOL_RANGE_UNSPECIFIED;
};

// Two 64x48 frames encoded with `stream` tags and muxed into `path` with `container` tags.
bool writeMovie(const std::filesystem::path& path, const Tags& stream, const Tags& container) {
    const auto* codec = avcodec_find_encoder_by_name("libopenh264");
    if (codec == nullptr)
        return false;
    AVCodecContext* encoder = avcodec_alloc_context3(codec);
    AVFormatContext* format = nullptr;
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    bool written = false;
    do {
        if (encoder == nullptr || frame == nullptr || packet == nullptr)
            break;
        encoder->width = 64;
        encoder->height = 48;
        encoder->pix_fmt = AV_PIX_FMT_YUV420P;
        encoder->time_base = {1, 24};
        encoder->framerate = {24, 1};
        encoder->gop_size = 24;
        encoder->bit_rate = 400000;
        encoder->color_primaries = stream.primaries;
        encoder->color_trc = stream.transfer;
        encoder->colorspace = stream.matrix;
        encoder->color_range = stream.range;
        if (avformat_alloc_output_context2(&format, nullptr, nullptr, path.string().c_str()) < 0 ||
            format == nullptr)
            break;
        if ((format->oformat->flags & AVFMT_GLOBALHEADER) != 0)
            encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(encoder, codec, nullptr) < 0)
            break;
        AVStream* track = avformat_new_stream(format, nullptr);
        if (track == nullptr || avcodec_parameters_from_context(track->codecpar, encoder) < 0)
            break;
        track->time_base = encoder->time_base;
        // What the container is told, which is the point of the test.
        track->codecpar->color_primaries = container.primaries;
        track->codecpar->color_trc = container.transfer;
        track->codecpar->color_space = container.matrix;
        track->codecpar->color_range = container.range;
        if (avio_open(&format->pb, path.string().c_str(), AVIO_FLAG_WRITE) < 0 ||
            avformat_write_header(format, nullptr) < 0)
            break;
        frame->format = encoder->pix_fmt;
        frame->width = encoder->width;
        frame->height = encoder->height;
        if (av_frame_get_buffer(frame, 0) < 0)
            break;
        bool failed = false;
        const auto drain = [&] {
            while (avcodec_receive_packet(encoder, packet) >= 0) {
                av_packet_rescale_ts(packet, encoder->time_base, track->time_base);
                packet->stream_index = track->index;
                failed = failed || av_interleaved_write_frame(format, packet) < 0;
                av_packet_unref(packet);
            }
        };
        for (int index = 0; index < 2 && !failed; ++index) {
            if (av_frame_make_writable(frame) < 0) {
                failed = true;
                break;
            }
            for (int plane = 0; plane < 3; ++plane) {
                const int rows = plane == 0 ? frame->height : frame->height / 2;
                for (int row = 0; row < rows; ++row)
                    std::memset(
                        frame->data[plane] +
                            static_cast<std::ptrdiff_t>(row) * frame->linesize[plane],
                        96 + 32 * index + 8 * plane,
                        static_cast<std::size_t>(plane == 0 ? frame->width : frame->width / 2));
            }
            frame->pts = index;
            failed = avcodec_send_frame(encoder, frame) < 0;
            drain();
        }
        failed = failed || avcodec_send_frame(encoder, nullptr) < 0;
        drain();
        written = !failed && av_write_trailer(format) >= 0;
    } while (false);
    if (format != nullptr) {
        if (format->pb != nullptr)
            avio_closep(&format->pb);
        avformat_free_context(format);
    }
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&encoder);
    return written;
}

void run(const std::filesystem::path& folder, const std::string_view extension) {
    using bloom::media::ffmpeg::h264StreamTagFailure;
    const auto name = [&](const std::string_view what) {
        return std::string{extension} + ": " + std::string{what};
    };
    const auto path = folder / ("stream-color-tags." + std::string{extension});
    const Tags rec709{AVCOL_PRI_BT709, AVCOL_TRC_BT709, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG};
    const auto check = [&](const Tags& stream, const Tags& container, const AVColorRange requested,
                           const std::string_view fragment, const std::string_view what) {
        if (!writeMovie(path, stream, container)) {
            expect(false, name("a movie is written for: " + std::string{what}));
            return;
        }
        const auto failure = h264StreamTagFailure(path.string(), 0, requested);
        if (fragment.empty())
            expect(failure.empty(), name(std::string{what} + " (got: " + failure + ")"));
        else
            expect(failure.find("differ from the container's") != std::string::npos &&
                       failure.find(fragment) != std::string::npos,
                   name(std::string{what} + " (got: " + failure + ")"));
    };

    check(rec709, rec709, AVCOL_RANGE_MPEG, {}, "equal Rec.709 limited tags pass");
    check({}, {}, AVCOL_RANGE_MPEG, {}, "a stream and a container that both state nothing pass");

    auto full = rec709;
    full.range = AVCOL_RANGE_JPEG;
    check(full, full, AVCOL_RANGE_JPEG, {}, "equal full-range tags pass");
    // A QuickTime color box has no place for the range, so only the stream can be wrong here.
    check(full, rec709, AVCOL_RANGE_MPEG, "stream primaries=1 transfer=1 matrix=1 range=full",
          "a full-range stream in a limited-range export is refused");
    check(rec709, rec709, AVCOL_RANGE_JPEG, "requested range full",
          "a limited-range stream in a full-range export is refused");

    auto wide = rec709;
    wide.primaries = AVCOL_PRI_BT2020;
    check(rec709, wide, AVCOL_RANGE_MPEG, "container primaries=9",
          "a container with other primaries than its stream is refused");
    auto pq = rec709;
    pq.transfer = AVCOL_TRC_SMPTE2084;
    check(rec709, pq, AVCOL_RANGE_MPEG, "container primaries=1 transfer=16",
          "a container with another transfer than its stream is refused");
    auto ncl = rec709;
    ncl.matrix = AVCOL_SPC_BT2020_NCL;
    check(ncl, rec709, AVCOL_RANGE_MPEG, "stream primaries=1 transfer=1 matrix=9",
          "a stream with another matrix than its container is refused");
    check({}, rec709, AVCOL_RANGE_MPEG, "stream primaries=2 transfer=2 matrix=2",
          "a stream that states nothing in a tagged container is refused");
    check(rec709, {}, AVCOL_RANGE_MPEG, "container primaries=2 transfer=2 matrix=2",
          "a tagged stream in a container that states nothing is refused");

    expect(!h264StreamTagFailure((folder / "missing.mov").string(), 0, AVCOL_RANGE_MPEG).empty(),
           name("a file that cannot be opened is not a pass"));
    expect(!h264StreamTagFailure(path.string(), 5, AVCOL_RANGE_MPEG).empty(),
           name("a stream that does not exist is not a pass"));
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}
} // namespace

int main() {
    std::error_code error;
    const auto folder = std::filesystem::temp_directory_path() / "bloom stream color tags";
    std::filesystem::create_directories(folder, error);
    run(folder, "mov");
    run(folder, "mp4");
    std::filesystem::remove_all(folder, error);
    if (failures != 0) {
        std::cerr << failures << " stream color tag expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: H.264 stream and container color tags\n";
    return 0;
}
