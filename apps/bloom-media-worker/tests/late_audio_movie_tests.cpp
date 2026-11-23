// #789: a movie whose audio starts after its video plays that audio at its time, in preview and in
// export, with silence before it and nothing lost at the end. The clip's audio length is where the
// audio ends measured from the video's start. A gap in the middle of an audio stream is still
// refused.
//
// The movies are generated here with libavformat: two seconds of 64x32 ProRes at 24 fps, and
// stereo 16-bit PCM that starts 0.5 s after the video and ends at 1.75 s (an empty edit leads the
// audio track, as some cameras and screen recorders write it). Every audio sample is non-zero and
// differs from its neighbours, so a misplaced sample shows. A Matroska file with the same streams
// whose audio jumps 0.5 s ahead in the middle is the gap a reader must still refuse.

#include "fixed_memory_budget.hpp"
#include "unique_temp_dir.hpp"
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/export_queue_operations.hpp>
#include <bloom/commands/layer_drop_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/host/export_presets.hpp>
#include <bloom/host/export_queue.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/project_session.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/host/sequence_export_runner.hpp>
#include <bloom/media/audio/audio.hpp>
#include <bloom/media/video/audio.hpp>
#include <bloom/media/video/session.hpp>
#include <bloom/output/media_output.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
}

namespace {
using namespace bloom;
using document::ExportFormat;

int failures = 0;
void expect(const bool condition, const std::string& message) {
    std::cout << (condition ? "PASS: " : "FAIL: ") << message << '\n';
    if (!condition)
        ++failures;
}

constexpr int kRate = 48000;
constexpr std::int64_t kVideoFrames = 48;    // two seconds at 24 fps
constexpr std::int64_t kAudioStart = 24000;  // 0.5 s after the video's first frame
constexpr std::int64_t kAudioFrames = 60000; // 1.25 s, so the audio ends at 1.75 s
constexpr std::int64_t kAudioEnd = kAudioStart + kAudioFrames;
constexpr int kPacketFrames = 1024;

// Sample i of the sound (counted from the sound's own first sample) in channel c, as 16-bit PCM:
// never zero, and each sample differs from the next by 10 steps or more.
std::int16_t sample(const std::int64_t i, const int c) {
    return static_cast<std::int16_t>((c == 0 ? 1000 : -1000) + (c == 0 ? 10 : -10) * (i % 2000));
}
float decoded(const std::int64_t i, const int c) {
    return static_cast<float>(sample(i, c)) / 32768.0F;
}

// Writes a movie of two seconds of ProRes and the stereo sound. The audio packets of 1024 frames
// start at the given sample on the movie's timeline; with a gap, the packets after the first
// gapAfter frames of sound are placed that many samples later, a hole in the middle of the stream.
bool writeMovie(const std::filesystem::path& path, const char* container,
                const std::int64_t audioStart, const std::int64_t gapAfter = -1,
                const std::int64_t gap = 0) {
    AVFormatContext* format = nullptr;
    if (avformat_alloc_output_context2(&format, nullptr, container, path.string().c_str()) < 0)
        return false;
    struct Close final {
        AVFormatContext* format;
        AVCodecContext* codec = nullptr;
        AVFrame* frame = nullptr;
        AVPacket* packet = nullptr;
        ~Close() {
            av_packet_free(&packet);
            av_frame_free(&frame);
            avcodec_free_context(&codec);
            if (format && format->pb)
                avio_closep(&format->pb);
            avformat_free_context(format);
        }
    } close{format};
    const auto* encoder = avcodec_find_encoder_by_name("prores_ks");
    if (!encoder)
        return false;
    close.codec = avcodec_alloc_context3(encoder);
    if (!close.codec)
        return false;
    auto& codec = *close.codec;
    codec.width = 64;
    codec.height = 32;
    codec.pix_fmt = AV_PIX_FMT_YUV422P10LE;
    codec.time_base = {1, 24};
    codec.framerate = {24, 1};
    if (format->oformat->flags & AVFMT_GLOBALHEADER)
        codec.flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(&codec, encoder, nullptr) < 0)
        return false;
    auto* video = avformat_new_stream(format, nullptr);
    auto* audio = avformat_new_stream(format, nullptr);
    if (!video || !audio || avcodec_parameters_from_context(video->codecpar, &codec) < 0)
        return false;
    video->time_base = {1, 24};
    audio->time_base = {1, kRate};
    auto& sound = *audio->codecpar;
    sound.codec_type = AVMEDIA_TYPE_AUDIO;
    sound.codec_id = AV_CODEC_ID_PCM_S16LE;
    sound.sample_rate = kRate;
    sound.format = AV_SAMPLE_FMT_S16;
    sound.bits_per_coded_sample = 16;
    sound.block_align = 4;
    sound.bit_rate = 16LL * 2 * kRate;
    av_channel_layout_default(&sound.ch_layout, 2);
    if (avio_open(&format->pb, path.string().c_str(), AVIO_FLAG_WRITE) < 0 ||
        avformat_write_header(format, nullptr) < 0)
        return false;
    close.packet = av_packet_alloc();
    close.frame = av_frame_alloc();
    if (!close.packet || !close.frame)
        return false;
    auto& frame = *close.frame;
    frame.format = codec.pix_fmt;
    frame.width = codec.width;
    frame.height = codec.height;
    if (av_frame_get_buffer(&frame, 0) < 0)
        return false;
    const auto drain = [&] {
        while (avcodec_receive_packet(&codec, close.packet) >= 0) {
            av_packet_rescale_ts(close.packet, codec.time_base, video->time_base);
            close.packet->stream_index = video->index;
            if (av_interleaved_write_frame(format, close.packet) < 0)
                return false;
        }
        return true;
    };
    // The audio packets, written in time order with the pictures.
    std::int64_t written = 0;
    const auto writeAudioUntil = [&](const std::int64_t until) {
        while (written < kAudioFrames) {
            auto at = audioStart + written;
            if (gapAfter >= 0 && written >= gapAfter)
                at += gap;
            if (at >= until)
                return true;
            const auto count = std::min<std::int64_t>(kPacketFrames, kAudioFrames - written);
            if (av_new_packet(close.packet, static_cast<int>(count * 4)) < 0)
                return false;
            auto* bytes = close.packet->data;
            for (std::int64_t i = 0; i < count; ++i)
                for (int c = 0; c < 2; ++c) {
                    const auto value = static_cast<std::uint16_t>(sample(written + i, c));
                    *bytes++ = static_cast<std::uint8_t>(value & 0xFFU);
                    *bytes++ = static_cast<std::uint8_t>(value >> 8U);
                }
            close.packet->pts = at;
            close.packet->dts = at;
            close.packet->duration = count;
            close.packet->flags |= AV_PKT_FLAG_KEY;
            av_packet_rescale_ts(close.packet, {1, kRate}, audio->time_base);
            close.packet->stream_index = audio->index;
            if (av_interleaved_write_frame(format, close.packet) < 0)
                return false;
            written += count;
        }
        return true;
    };
    for (std::int64_t n = 0; n < kVideoFrames; ++n) {
        if (av_frame_make_writable(&frame) < 0)
            return false;
        for (int plane = 0; plane < 3; ++plane) {
            const int width = plane == 0 ? codec.width : codec.width / 2;
            for (int y = 0; y < codec.height; ++y) {
                auto* row =
                    reinterpret_cast<std::uint16_t*>(frame.data[plane] + y * frame.linesize[plane]);
                for (int x = 0; x < width; ++x)
                    row[x] = static_cast<std::uint16_t>(
                        plane == 0 ? 64 + ((x * 13 + y * 7 + n * 5) % 876) : 512);
            }
        }
        frame.pts = n;
        if (!writeAudioUntil(n * kRate / 24) || avcodec_send_frame(&codec, &frame) < 0 || !drain())
            return false;
    }
    if (avcodec_send_frame(&codec, nullptr) < 0 || !drain() ||
        !writeAudioUntil(std::numeric_limits<std::int64_t>::max()))
        return false;
    return av_write_trailer(format) >= 0;
}

// Reads frames stereo frames from the given first frame through the windowed reader, block by
// block; empty on failure.
std::vector<float> readWindow(const media::audio::AudioBuffer& buffer, const std::uint64_t first,
                              const std::size_t frames) {
    std::vector<float> interleaved(frames * 2U);
    constexpr std::size_t kBlock = 1024;
    for (std::size_t at = 0; at < frames; at += kBlock) {
        const auto count = std::min(kBlock, frames - at);
        const auto result = buffer.stream->readInterleaved(
            first + at, std::span<float>(interleaved).subspan(at * 2U, count * 2U));
        if (!result.hasValue() || *result.value() != count)
            return {};
    }
    return interleaved;
}

// The first movie frame at which interleaved stereo starting at movie frame first differs from
// the movie's audio: silence before the sound starts and after it ends, the sound between. -1
// when every frame matches.
std::int64_t firstWrong(std::span<const float> interleaved, const std::int64_t first) {
    const auto frames = static_cast<std::int64_t>(interleaved.size() / 2U);
    for (std::int64_t i = 0; i < frames; ++i) {
        const auto at = first + i;
        for (int c = 0; c < 2; ++c) {
            const auto want =
                at < kAudioStart || at >= kAudioEnd ? 0.0F : decoded(at - kAudioStart, c);
            if (interleaved[static_cast<std::size_t>(i * 2 + c)] != want)
                return at;
        }
    }
    return -1;
}

void testPreviewReader(const std::filesystem::path& movie) {
    auto session = std::make_shared<media::video::VideoDecodeSession>(movie);
    const auto probed = session->probe();
    const auto* probe = std::get_if<media::provider::ProbeResult>(&probed);
    expect(probe != nullptr, "preview: the worker probes the movie");
    if (!probe)
        return;
    const auto* stream = media::video::audioStreamAt(*probe, 0);
    expect(stream && stream->sampleRate == kRate && stream->channelLayout.size() == 2U,
           "preview: one 48 kHz stereo audio stream");
    if (!stream)
        return;
    const auto length = media::video::audioClipFrameCount(*stream);
    expect(length == static_cast<std::uint64_t>(kAudioEnd),
           "the clip's audio length is where the audio ends, measured from the video's start: " +
               std::to_string(length.value_or(0)) + " of " + std::to_string(kAudioEnd) + " frames");

    // Preview analysis and playback read a movie's audio through this window, block by block.
    auto windowed = media::video::openWindowedVideoAudio(session, *probe);
    const auto* buffer = std::get_if<media::audio::AudioBuffer>(&windowed);
    expect(buffer && buffer->stream && buffer->frames == static_cast<std::uint64_t>(kAudioEnd),
           "preview: the windowed reader opens the whole audio");
    if (!buffer || !buffer->stream)
        return;
    const auto all = readWindow(*buffer, 0, static_cast<std::size_t>(kAudioEnd));
    expect(!all.empty() && !buffer->stream->diagnostic(),
           "preview: every block decodes, with no diagnostic");
    if (!all.empty())
        expect(firstWrong(all, 0) == -1,
               "preview: silence until 0.5 s, then every sample of the sound at its time, to its "
               "end (first wrong frame " +
                   std::to_string(firstWrong(all, 0)) + ")");
    // Reads that start inside the silence, across the sound's start, inside the sound and at its
    // end land on the same samples.
    for (const std::uint64_t first :
         {std::uint64_t{0}, std::uint64_t{12000}, std::uint64_t{23000}, std::uint64_t{40017},
          static_cast<std::uint64_t>(kAudioEnd - 4096)}) {
        const auto read = readWindow(*buffer, first, 4096);
        expect(!read.empty() && firstWrong(read, static_cast<std::int64_t>(first)) == -1,
               "preview: a read from frame " + std::to_string(first) + " lands on its samples");
    }
    // The whole clip in memory.
    auto whole = media::video::decodeAudioClip(*session, *probe, 1U << 20U);
    const auto* clip = std::get_if<media::audio::AudioBuffer>(&whole);
    expect(clip && clip->channels == 2U && clip->frames == static_cast<std::uint64_t>(kAudioEnd),
           "the whole clip decodes, " + std::to_string(clip ? clip->frames : 0) + " frames");
    if (clip && clip->planes.size() == 2U && clip->planes[0].size() == clip->frames &&
        clip->planes[1].size() == clip->frames) {
        std::vector<float> interleaved(static_cast<std::size_t>(clip->frames) * 2U);
        for (std::size_t i = 0; i < clip->frames; ++i)
            for (std::size_t c = 0; c < 2U; ++c)
                interleaved[i * 2U + c] = clip->planes[c][i];
        expect(firstWrong(interleaved, 0) == -1, "the whole clip holds the sound at its time");
    }
}

void testGapRefused(const std::filesystem::path& movie) {
    auto session = std::make_shared<media::video::VideoDecodeSession>(movie);
    const auto probed = session->probe();
    const auto* probe = std::get_if<media::provider::ProbeResult>(&probed);
    expect(probe != nullptr, "gap: the worker probes the movie");
    if (!probe)
        return;
    auto whole = media::video::decodeAudioClip(*session, *probe, 1U << 20U);
    const auto* refused = std::get_if<media::provider::Unavailable>(&whole);
    expect(refused && refused->detail.find("Audio timestamp gap") != std::string::npos,
           "gap: audio that jumps ahead in the middle of its stream is refused" +
               (refused ? ": " + refused->detail : std::string{}));
    auto windowed = media::video::openWindowedVideoAudio(session, *probe);
    if (const auto* buffer = std::get_if<media::audio::AudioBuffer>(&windowed);
        buffer && buffer->stream) {
        const auto across = readWindow(*buffer, 0, 48000);
        expect(across.empty() && buffer->stream->diagnostic().has_value(),
               "gap: a windowed read across the gap fails with a diagnostic");
    } else {
        expect(false, "gap: the windowed reader opens the movie");
    }
}

// The export route: the movie imported and dropped into a three-second composition.
document::Snapshot currentSnapshot(const host::ProjectSession& session) {
    const auto decoded = session.decodedSnapshot();
    return decoded.snapshot();
}

host::ExportEncoderStatus linuxEncoders() {
    host::ExportEncoderStatus status;
    status.probed = true;
    status.mediaWorker = true;
    status.h264Software = output::H264EncoderV1::OpenH264;
    status.openh264 = host::ExportEncoderStatus::OpenH264::Installed;
    return status;
}

struct Fixture {
    std::filesystem::path folder;
    host::ProjectSessionIdentitySource identity;
    host::ProjectSession session;
    runtime::NodeDefinitionRegistry registry;
    runtime::SnapshotCompiler compiler{registry};
    runtime::TaskScheduler scheduler;
    host::PublicationCoordinator publications = [] {
        auto result = host::PublicationCoordinator::create();
        if (!result)
            throw std::runtime_error("publication coordinator");
        return std::move(*result);
    }();
    platform::StagedArtifactCoordinator artifacts = [] {
        auto result = platform::StagedArtifactCoordinator::create({});
        if (!result)
            throw std::runtime_error("artifact coordinator");
        return std::move(result).takeCoordinator();
    }();
    std::shared_ptr<output::ExportResourceLedgerV1> ledger =
        std::make_shared<output::ExportResourceLedgerV1>();
    std::shared_ptr<host::GpuExportProvider> gpu;
    std::shared_ptr<host::SequenceExportRetirementOwnerV1> retirement =
        host::SequenceExportRetirementOwnerV1::create();
    std::shared_ptr<host::ExportPresetCatalogue> presets;
    std::unique_ptr<host::ExportQueueService> service;

    static host::ProjectSession makeSession(host::ProjectSessionIdentitySource& identity,
                                            const std::filesystem::path& folder) {
        auto format = document::CompositionFormat::create(64, 32);
        auto created = document::makeNewProject("Late audio", "Main",
                                                core::RationalTime::fromInteger(3), *format);
        auto path = host::ProjectDisplayPath::create(folder / "late-audio.bloom");
        auto result = host::ProjectSession::createDecoded(
            identity, {.project = std::move(created.project),
                       .colorSettings = document::makeBloomNeutralColorSettingsV1({}),
                       .editability = host::DecodedProjectEditability::Editable,
                       .displayPath = std::move(path),
                       .persistedAllocatorHighWater = std::nullopt});
        if (!result)
            throw std::runtime_error("session fixture");
        return std::move(result).takeSession();
    }

    explicit Fixture(std::filesystem::path where)
        : folder(std::move(where)), session(makeSession(identity, folder)) {
        if (!runtime::registerBuiltInNodeDefinitions(registry))
            throw std::runtime_error("registry");
        registry.freeze();
        runtime::GpuProcessFrameEvaluatorOptions options;
        options.hostMemorySampler = bloom::test_support::fixedMemoryBudgetSample;
        options.enabled = false;
        gpu = host::GpuExportProvider::create(std::move(options));
        gpu->prepare(scheduler);
        presets = host::ExportPresetCatalogue::create(
            {.userDataDirectory = std::nullopt, .initialEncoders = linuxEncoders()});
        service = std::make_unique<host::ExportQueueService>(
            host::ExportQueueServiceDependencies{.session = session,
                                                 .scheduler = scheduler,
                                                 .compiler = compiler,
                                                 .publications = publications,
                                                 .artifacts = artifacts,
                                                 .ledger = ledger,
                                                 .gpu = gpu,
                                                 .retirement = retirement,
                                                 .presets = presets,
                                                 .limits = {},
                                                 .displayProcessor = nullptr,
                                                 .scratchDirectory = folder / "scratch",
                                                 .backend = nullptr});
        std::filesystem::create_directories(folder / "scratch");
        std::filesystem::create_directories(folder / "Exports");
    }
    ~Fixture() {
        service->beginShutdown(host::ExportStopCause::ApplicationQuit);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!service->quiescent() && std::chrono::steady_clock::now() < deadline) {
            service->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        service.reset();
        scheduler.beginShutdown();
        retirement->beginShutdown();
        while (!scheduler.isQuiescent() || !retirement->retirementComplete())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        while (!retirement->collect())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        (void)gpu->shutdownAndWait(std::chrono::seconds(10));
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    document::CompositionId composition() const {
        return currentSnapshot(session).project().compositions().front().id();
    }
    void execute(commands::Transaction transaction, const std::string& what) {
        if (!session.execute(std::move(transaction)).changed())
            throw std::runtime_error(what);
    }
    document::ExportJobId addJob(document::ExportOutputSettings settings, std::string path) {
        document::ExportJob job;
        job.composition = composition();
        job.render.range = {document::ExportRangeKind::Composition, 0, 0};
        document::ExportOutput output;
        output.presetId = "test";
        output.settings = std::move(settings);
        output.destination = {"project-relative", std::move(path),
                              document::ExportOverwritePolicy::Replace};
        job.outputs = {output};
        commands::Transaction transaction("Add export job");
        transaction.emplace<commands::AddExportJob>(job);
        const auto result = session.execute(std::move(transaction));
        if (!result.changed())
            throw std::runtime_error("add job");
        return *result.command->outputId<document::ExportJobId>(commands::kExportJobOutput);
    }
    std::optional<host::ExportJobView> view(const document::ExportJobId id) const {
        service->pump();
        for (const auto& job : service->snapshot()->jobs)
            if (job.id == id)
                return job;
        return std::nullopt;
    }
    template <typename Predicate> bool pumpUntil(Predicate&& done, const int seconds) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < deadline) {
            service->pump();
            if (done())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }
    std::optional<host::ExportJobView> run(const document::ExportJobId job,
                                           const std::string& name) {
        expect(pumpUntil([&] { return service->canStart(job).available; }, 20),
               name + ": the job becomes startable");
        const document::ExportJobId ids[] = {job};
        expect(service->start(ids).accepted, name + ": start");
        expect(pumpUntil([&] { return host::isTerminal(view(job)->state); }, 240),
               name + ": the job finishes");
        if (const auto finished = view(job); finished && finished->failure)
            for (const auto& entry : service->log(job, 0).entries)
                std::cout << "  " << name << " log: " << entry.code << ": " << entry.message
                          << '\n';
        return view(job);
    }
};

bool published(const std::optional<host::ExportJobView>& view) {
    return view && (view->state == host::ExportJobState::Done ||
                    view->state == host::ExportJobState::DoneWithWarnings);
}

void testExport(const std::filesystem::path& folder, const std::filesystem::path& movie) {
    std::filesystem::create_directories(folder);
    Fixture f(folder);
    {
        commands::Transaction import("Import movie");
        import.emplace<commands::ImportAssets>(std::vector<std::filesystem::path>{movie}, folder);
        f.execute(std::move(import), "import the movie");
    }
    std::vector<commands::LayerDropItem> items;
    for (const auto& asset : currentSnapshot(f.session).project().assets())
        if (asset.kind == document::AssetKind::Video)
            items.emplace_back(asset.id);
    expect(items.size() == 1U, "export: one movie asset");
    {
        commands::Transaction layers("Add movie layer");
        layers.emplace<commands::AddDroppedLayers>(f.composition(), items,
                                                   commands::LayerDropPlacement{});
        f.execute(std::move(layers), "add the movie layer");
    }
    document::ExportOutputSettings output;
    output.format = ExportFormat::ProResMov;
    output.codecProfile = "hq";
    output.includeAudio = true;
    const auto finished = f.run(f.addJob(output, "Exports/late-audio.mov"), "export");
    expect(published(finished), "export: the movie with late audio exports and publishes");
    if (!published(finished))
        return;
    const auto path = folder / "Exports" / "late-audio.mov";
    media::video::VideoDecodeSession decoder(path);
    const auto probed = decoder.probe();
    const auto* probe = std::get_if<media::provider::ProbeResult>(&probed);
    expect(probe != nullptr, "export: the published movie probes");
    if (!probe)
        return;
    auto sound = media::video::decodeAudioClip(decoder, *probe, 1U << 20U);
    const auto* clip = std::get_if<media::audio::AudioBuffer>(&sound);
    constexpr std::uint64_t kExported = 3U * kRate;
    const bool whole = clip && clip->channels == 2U && clip->planes.size() == 2U &&
                       clip->frames == kExported && clip->planes[0].size() == kExported &&
                       clip->planes[1].size() == kExported;
    expect(whole, "export: three seconds of stereo audio (" +
                      std::to_string(clip ? clip->frames : 0) + " frames)");
    if (!whole)
        return;
    std::vector<float> interleaved(static_cast<std::size_t>(kExported) * 2U);
    for (std::size_t i = 0; i < kExported; ++i)
        for (std::size_t c = 0; c < 2U; ++c)
            interleaved[i * 2U + c] = clip->planes[c][i];
    const auto wrong = firstWrong(interleaved, 0);
    expect(wrong == -1, "export: silence until 0.5 s, every sample of the sound at its time until "
                        "1.75 s, silence after (first wrong frame " +
                            std::to_string(wrong) + ")");
}
} // namespace

int main() {
    const auto folder =
        bloom::test_support::processTempDirectory() /
        ("bloom-late-audio-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    struct Remove final {
        std::filesystem::path folder;
        ~Remove() {
            std::error_code ignored;
            std::filesystem::remove_all(folder, ignored);
        }
    } remove{folder};
    try {
        const auto late = folder / "late-audio.mov";
        const auto gap = folder / "gap-audio.mkv";
        expect(writeMovie(late, "mov", kAudioStart), "a movie whose audio starts 0.5 s late");
        expect(writeMovie(gap, "matroska", 0, 12 * kPacketFrames, kRate / 2),
               "a movie whose audio jumps 0.5 s ahead after 12 packets");
        testPreviewReader(late);
        testGapRefused(gap);
        testExport(folder / "export", late);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures == 0)
        std::cout << "late audio: ok\n";
    return failures == 0 ? 0 : 1;
}
