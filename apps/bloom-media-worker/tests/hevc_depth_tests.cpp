// Issue #408: an independent read of the software HEVC grey ramps bloom.media.encode writes. A
// Main 10 request must reopen as HEVC Main 10, 4:2:0 10-bit, tagged 'hvc1' with the BT.709 limited
// colour it was given, and its decoded luma must carry more distinct codes than 8 bits can hold;
// the 8-bit Main ramp is the control that proves the count tells the two apart.
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}
namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    std::cout << (ok ? "PASS: " : "FAIL: ") << what << '\n';
    if (!ok)
        ++failures;
}
struct Read {
    bool opened = false;
    int profile = AV_PROFILE_UNKNOWN;
    AVPixelFormat format = AV_PIX_FMT_NONE;
    std::uint32_t tag = 0;
    AVColorPrimaries primaries = AVCOL_PRI_UNSPECIFIED;
    AVColorTransferCharacteristic transfer = AVCOL_TRC_UNSPECIFIED;
    AVColorSpace matrix = AVCOL_SPC_UNSPECIFIED;
    AVColorRange range = AVCOL_RANGE_UNSPECIFIED;
    std::size_t lumaCodes = 0;
};
// Probes the first video stream and counts the distinct luma codes of the first decoded frame's
// middle row.
Read read(const std::filesystem::path& path) {
    Read result;
    AVFormatContext* format = nullptr;
    if (avformat_open_input(&format, path.string().c_str(), nullptr, nullptr) < 0)
        return result;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    const int index = avformat_find_stream_info(format, nullptr) >= 0
                          ? av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0)
                          : -1;
    if (index >= 0 && packet && frame) {
        const auto& parameters = *format->streams[index]->codecpar;
        result.opened = true;
        result.profile = parameters.profile;
        result.format = static_cast<AVPixelFormat>(parameters.format);
        result.tag = parameters.codec_tag;
        result.primaries = parameters.color_primaries;
        result.transfer = parameters.color_trc;
        result.matrix = parameters.color_space;
        result.range = parameters.color_range;
        const auto* decoder = avcodec_find_decoder(parameters.codec_id);
        codec = decoder ? avcodec_alloc_context3(decoder) : nullptr;
        if (codec && avcodec_parameters_to_context(codec, &parameters) >= 0 &&
            avcodec_open2(codec, decoder, nullptr) >= 0) {
            bool decoded = false;
            while (!decoded && av_read_frame(format, packet) >= 0) {
                if (packet->stream_index == index && avcodec_send_packet(codec, packet) >= 0)
                    decoded = avcodec_receive_frame(codec, frame) >= 0;
                av_packet_unref(packet);
            }
            if (!decoded && avcodec_send_packet(codec, nullptr) >= 0)
                decoded = avcodec_receive_frame(codec, frame) >= 0;
            if (decoded) {
                const auto* layout = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
                const bool wide = layout != nullptr && layout->comp[0].depth > 8;
                const auto* row = frame->data[0] + static_cast<std::ptrdiff_t>(frame->height / 2) *
                                                       frame->linesize[0];
                std::set<unsigned> codes;
                for (int x = 0; x < frame->width; ++x)
                    codes.insert(wide ? static_cast<unsigned>(row[2 * x]) |
                                            (static_cast<unsigned>(row[2 * x + 1]) << 8U)
                                      : static_cast<unsigned>(row[x]));
                result.lumaCodes = codes.size();
            }
        }
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec);
    avformat_close_input(&format);
    return result;
}
} // namespace
int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: bloom_media_hevc_depth_test <encode fixture directory>\n";
        return 2;
    }
    const std::filesystem::path directory = argv[1];
    for (const auto* container : {"mov", "mp4"}) {
        const auto ten = read(directory / (std::string("export-hevc-main10-ramp.") + container));
        const auto eight = read(directory / (std::string("export-hevc-main-ramp.") + container));
        const std::string name = std::string(" (") + container + ")";
        check(ten.opened && eight.opened, "both software HEVC ramps reopen" + name);
        check(ten.profile == AV_PROFILE_HEVC_MAIN_10 && ten.format == AV_PIX_FMT_YUV420P10LE,
              "the Main 10 request is HEVC Main 10, yuv420p10le" + name);
        check(eight.profile == AV_PROFILE_HEVC_MAIN && eight.format == AV_PIX_FMT_YUV420P,
              "the Main request is HEVC Main, yuv420p" + name);
        check(ten.tag == MKTAG('h', 'v', 'c', '1') && eight.tag == MKTAG('h', 'v', 'c', '1'),
              "both are tagged hvc1" + name);
        check(ten.primaries == AVCOL_PRI_BT709 && ten.transfer == AVCOL_TRC_BT709 &&
                  ten.matrix == AVCOL_SPC_BT709 && ten.range == AVCOL_RANGE_MPEG,
              "the Main 10 stream carries BT.709 limited-range colour tags" + name);
        std::cout << "luma codes" << name << ": Main 10 " << ten.lumaCodes << ", Main "
                  << eight.lumaCodes << '\n';
        check(ten.lumaCodes > 256, "the Main 10 ramp decodes to more than 256 luma codes" + name);
        check(eight.lumaCodes > 0 && eight.lumaCodes <= 256,
              "the 8-bit control decodes to at most 256 luma codes" + name);
    }
    return failures == 0 ? 0 : 1;
}
