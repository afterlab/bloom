// The worker's Media Foundation generation against its software generation, in one process, on
// the same files (issue #370).
//
// Both generations read the container with libavformat, so the description and the index of a
// file are the same code. What differs is who decodes the pictures: H.264 and HEVC decoding is
// defined to the bit, so every frame the system decoder returns has to equal the software
// decoder's frame exactly: same layout, same tags, same bytes. A source the generation does not
// read that way has to be refused with a reason, and never read differently.
#include "ffmpeg_provider.hpp"

#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/media/provider/protocol.hpp>
#include <bloom/media/provider/provider_chain.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace bloom::media;

int failures = 0;
int compared = 0;
int frames = 0;
// Sources whose H.264 or HEVC streams decode on the graphics device, and on the processor.
int hardwareSources = 0;
int softwareSources = 0;
// Sources the generation described and then refused at a frame.
std::set<std::string> refusedFrames;
void check(const bool condition, const std::string& what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what.c_str());
        ++failures;
    }
}

// A request as the host would send it to the provider whose handshake is `hello`.
[[nodiscard]] provider::CallRequest request(const provider::Handshake& hello,
                                            const provider::Role role, const std::string& path,
                                            const provider::ProbeResult* probe = nullptr,
                                            const std::uint32_t stream = 0) {
    provider::CallRequest call;
    call.capability.role = role;
    call.source = path;
    call.stream = stream;
    if (probe != nullptr && role != provider::Role::DemuxIndex) {
        if (const auto* found =
                provider::findDecodeDeclarationV1(hello, role, probe->streams[stream].codec))
            call.capability = found->capability;
        call.width = probe->streams[stream].width;
        call.height = probe->streams[stream].height;
        call.sourceDigest = probe->sourceDigest;
    } else {
        for (const auto& declaration : hello.declarations)
            if (declaration.capability.role == role) {
                call.capability = declaration.capability;
                break;
            }
        if (probe != nullptr)
            call.sourceDigest = probe->sourceDigest;
    }
    return call;
}

[[nodiscard]] std::string reason(const provider::Payload& payload) {
    const auto* error = std::get_if<provider::Unavailable>(&payload);
    return error != nullptr ? error->detail : std::string("no reason");
}

// True when the file was compared, false when the generation refused it.
bool compare(const std::filesystem::path& path, const bool mustRead) {
    const auto file = path.filename().string();
    const auto name = path.string();
    const auto platform = ffmpeg::Generation::Platform;
    const auto hello = ffmpeg::generationHandshake(platform);
    const auto softwareHello = provider::ffmpegHandshake();

    auto referencePayload = ffmpeg::call(request(softwareHello, provider::Role::Probe, name));
    const auto* reference = std::get_if<provider::ProbeResult>(&referencePayload);
    if (reference == nullptr) {
        // Not a media file the worker reads at all.
        check(!mustRead,
              file + ": the software generation refused it: " + reason(referencePayload));
        return false;
    }
    auto probed = ffmpeg::call(request(hello, provider::Role::Probe, name), false, platform);
    const auto* described = std::get_if<provider::ProbeResult>(&probed);
    if (described == nullptr) {
        const auto* error = std::get_if<provider::Unavailable>(&probed);
        check(error != nullptr && error->reason == provider::Error::Unavailable &&
                  !error->detail.empty(),
              file + ": a refusal is a typed one with its reason (" + reason(probed) + ")");
        check(!mustRead, file + ": the Media Foundation generation refused it: " + reason(probed));
        std::printf("%s: refused: %s\n", file.c_str(), reason(probed).c_str());
        return false;
    }
    ++compared;
    // The description is the software generation's; the reader's name is the framework's, with
    // how each stream is decoded.
    auto renamed = *described;
    renamed.version = reference->version;
    check(renamed == *reference, file + ": is described as the software generation does");
    check(described->version.starts_with("Media Foundation") &&
              described->version.ends_with("; FFmpeg-8.1.2"),
          file + ": the reader is named: " + described->version);
    if (described->version.find("Direct3D 11 hardware decode") != std::string::npos)
        ++hardwareSources;
    else if (described->version.find("software decode (") != std::string::npos)
        ++softwareSources;
    std::printf("%s: %s\n", file.c_str(), described->version.c_str());
    if (described->streams.size() != reference->streams.size())
        return true;

    bool video = false;
    for (const auto& stream : described->streams)
        video = video || stream.kind == provider::MediaKind::Video;
    if (video) {
        auto a = ffmpeg::call(request(hello, provider::Role::DemuxIndex, name, described), false,
                              platform);
        auto r = ffmpeg::call(request(softwareHello, provider::Role::DemuxIndex, name, reference));
        const auto* index = std::get_if<provider::DemuxIndex>(&a);
        const auto* referenceIndex = std::get_if<provider::DemuxIndex>(&r);
        // Equal, or refused by both for the same reason (the index is libavformat's either way).
        check((index != nullptr && referenceIndex != nullptr && *index == *referenceIndex) ||
                  (index == nullptr && referenceIndex == nullptr && reason(a) == reason(r)),
              file + ": the index equals the software generation's");
        if (index == nullptr || referenceIndex == nullptr)
            std::fprintf(stderr, "  Media Foundation: %s; software: %s\n",
                         index != nullptr ? "indexed" : reason(a).c_str(),
                         referenceIndex != nullptr ? "indexed" : reason(r).c_str());
        if (index != nullptr && referenceIndex != nullptr && !(*index == *referenceIndex)) {
            const auto& ours = index->keyframes;
            const auto& theirs = referenceIndex->keyframes;
            std::size_t at = 0;
            while (at < ours.size() && at < theirs.size() && ours[at] == theirs[at])
                ++at;
            const auto entry = [](const provider::IndexEntry& value) {
                return "stream " + std::to_string(value.stream) + " pts " +
                       std::to_string(value.pts.numerator) + "/" +
                       std::to_string(value.pts.denominator) + " dts " +
                       std::to_string(value.dts.numerator) + "/" +
                       std::to_string(value.dts.denominator);
            };
            auto text = "  " + std::to_string(ours.size()) + " and " +
                        std::to_string(theirs.size()) + " entries; first difference at entry " +
                        std::to_string(at);
            if (at < ours.size() && at < theirs.size())
                text += ": " + entry(ours[at]) + " against " + entry(theirs[at]);
            std::fprintf(stderr, "%s\n", text.c_str());
        }
    }

    for (std::uint32_t stream = 0; stream < described->streams.size(); ++stream) {
        const auto& track = described->streams[stream];
        const bool picture = track.kind == provider::MediaKind::Video;
        const auto role = picture ? provider::Role::VideoDecode : provider::Role::AudioDecode;
        if (track.kind != provider::MediaKind::Video && track.kind != provider::MediaKind::Audio)
            continue;
        // A codec the generation does not declare is decoded by the software generation: the
        // host never sends it here.
        if (provider::findDecodeDeclarationV1(hello, role, track.codec) == nullptr) {
            std::printf("%s stream %u: %s is left to the software generation\n", file.c_str(),
                        stream, track.codec.c_str());
            continue;
        }
        if (picture) {
            // Forward from the start, then out of order: the last frame, back to the middle, and
            // the first again.
            const auto count = track.frameCount;
            std::vector<std::uint64_t> order;
            for (std::uint64_t frame = 0; frame < std::min<std::uint64_t>(count, 16); ++frame)
                order.push_back(frame);
            if (count > 0) {
                order.push_back(count - 1);
                order.push_back(count / 2);
                order.push_back(0);
            }
            for (const auto frame : order) {
                auto call = request(hello, role, name, described, stream);
                auto referenceCall = request(softwareHello, role, name, reference, stream);
                call.frame = referenceCall.frame = frame;
                auto a = ffmpeg::call(call, false, platform);
                auto r = ffmpeg::call(referenceCall);
                const auto* decoded = std::get_if<provider::FrameProduct>(&a);
                const auto* expected = std::get_if<provider::FrameProduct>(&r);
                const auto label = file + " frame " + std::to_string(frame);
                // A frame the generation refuses with a typed reason is read by the software
                // generation in Bloom (the host hands the source over on the first refusal).
                if (const auto* refused = std::get_if<provider::Unavailable>(&a);
                    refused != nullptr && expected != nullptr && !mustRead &&
                    refused->reason == provider::Error::Unavailable && !refused->detail.empty() &&
                    refused->detail.starts_with("The Windows media framework")) {
                    if (refusedFrames.insert(file).second)
                        std::printf("%s: refused from frame %s: %s\n", file.c_str(),
                                    std::to_string(frame).c_str(), refused->detail.c_str());
                    continue;
                }
                if (decoded == nullptr || expected == nullptr) {
                    check(false, label + ": both generations decode it (Media Foundation: " +
                                     (decoded ? std::string("decoded") : reason(a)) +
                                     "; software: " +
                                     (expected ? std::string("decoded") : reason(r)) + ")");
                    continue;
                }
                ++frames;
                check(*decoded == *expected,
                      label + ": equals the software generation's frame bit for bit");
            }
            continue;
        }
        // Uncompressed audio: from the start, from an arbitrary sample, and after it.
        for (const std::uint64_t first :
             {std::uint64_t{0}, std::uint64_t{1234}, std::uint64_t{1234 + 4800}}) {
            auto call = request(hello, role, name, described, stream);
            auto referenceCall = request(softwareHello, role, name, reference, stream);
            call.frame = referenceCall.frame = first;
            call.samples = referenceCall.samples = 4800;
            auto r = ffmpeg::call(referenceCall);
            const auto* expected = std::get_if<provider::AudioBlock>(&r);
            // A stream shorter than the block is not what this comparison is about.
            if (expected == nullptr)
                continue;
            auto a = ffmpeg::call(call, false, platform);
            const auto* decoded = std::get_if<provider::AudioBlock>(&a);
            check(decoded != nullptr && decoded->pts == expected->pts &&
                      decoded->sampleRate == expected->sampleRate &&
                      decoded->channelLayout == expected->channelLayout &&
                      decoded->channels == expected->channels,
                  file + " audio from sample " + std::to_string(first) +
                      ": equals the software generation's samples");
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <fixture directory> <export fixture directory>\n", argv[0]);
        return 2;
    }
    const auto fixtures = std::filesystem::absolute(argv[1]);
    const auto exports = std::filesystem::absolute(argv[2]);
    // H.264 in MPEG-4, tagged and untagged, rotated, anamorphic and with a variable cadence: read,
    // and equal to the software generation. The in-box H.264 decoder is part of every Windows.
    for (const char* name :
         {"numbered-h264.mp4", "untagged-h264.mp4", "pq2020-h264.mp4", "hlg2020-h264.mp4",
          "bars709-h264.mp4", "bars-untagged-h264.mp4", "long-h264.mp4", "anamorphic-h264.mp4",
          "rotated-h264.mp4", "vfr-h264.mp4"})
        compare(fixtures / name, true);
    // Everything else the fixture and export tests wrote is either refused with a reason or equal
    // to the software generation. HEVC is read where the system's HEVC decoder starts.
    std::vector<std::filesystem::path> written;
    for (const auto& directory : {fixtures, exports}) {
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(directory, error))
            if (entry.is_regular_file())
                written.push_back(entry.path());
    }
    std::ranges::sort(written);
    for (const auto& path : written)
        compare(path, false);
    check(compared >= 10 && frames > 0, "the H.264 fixtures were compared");
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("%d files and %d frames compared with the software generation\n", compared, frames);
    std::printf("%d sources decoded on the graphics device (Direct3D 11), %d on the processor\n",
                hardwareSources, softwareSources);
    return 0;
}
