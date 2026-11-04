#ifdef BLOOM_FAKE_WORKER
#include "fake_provider.hpp"
#elif defined(BLOOM_VIDEOTOOLBOX_WORKER)
#include "videotoolbox_provider.hpp"
#include <bloom/media/provider/videotoolbox_manifest.hpp>
#else
#include "ffmpeg_provider.hpp"
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#if !defined(_WIN32)
#include <fcntl.h>
#endif
#endif
#if defined(_WIN32)
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#endif
#include "worker_io.hpp"
#include <bloom/platform/process_supervisor.hpp>
#include <bloom/platform/shared_frame_arena.hpp>
#include <chrono>
#include <cstdlib>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <memory>
#include <string_view>

int main(int argc, char** argv) {
    using namespace bloom::media;
    if (!bloom::platform::processWorkerBootstrap())
        return 2;
#if defined(_WIN32)
    // The protocol is binary: no CRLF translation or Ctrl-Z end-of-file on the pipes.
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1)
        return 2;
#endif
#if defined(__GLIBC__)
    // Decoder threads would otherwise each reserve a 64 MiB malloc arena inside the bounded
    // media-v1 address space.
    mallopt(M_ARENA_MAX, 4);
    // Frame planes and messages are megabytes each. Above the default threshold every one is a
    // fresh mapping whose pages fault in again on every frame (about 21,000 faults per 1080p
    // frame); kept in the heap, a freed frame's memory is reused by the next.
    mallopt(M_MMAP_THRESHOLD, 32 << 20);
    mallopt(M_TRIM_THRESHOLD, 256 << 20);
#endif
    bool retainedVideo = false;
    // Issue #474: the host's shared frame arena, installed as fd 6 for a retained decoder.
    std::unique_ptr<bloom::platform::WorkerFrameArena> arena;
#ifdef BLOOM_FAKE_WORKER
    (void)argc;
    (void)argv;
    const auto handshake = fake::handshake();
#elif defined(BLOOM_VIDEOTOOLBOX_WORKER)
    (void)argc;
    (void)argv;
    const auto handshake = provider::videoToolboxHandshake(videotoolbox::hardwareEncodeAvailable());
    videotoolbox::Encoder encoder;
#else
    bool hardware = false;
    bool hardwareDecode = false;
    auto decodeDevice = provider::HardwareDecodeDevice::Vaapi;
    bool nvenc = false;
    bool frameArena = false;
    int stagedOutput = -1;
    for (int i = 1; i < argc; ++i) {
        const auto argument = std::string_view(argv[i]);
        if (argument == "--probe-vaapi" && argc == 2)
            return ffmpeg::probeVaapiEncode() ? 0 : 11;
        if (argument == "--probe-nvenc" && argc == 2)
            return ffmpeg::probeNvencEncode() ? 0 : 11;
        if (argument == "--vaapi")
            hardware = true;
        else if (argument == "--nvenc")
            nvenc = true;
        else if (argument == "--vaapi-decode" && !hardwareDecode)
            hardwareDecode = true;
        else if (argument == "--nvdec-decode" && !hardwareDecode) {
            hardwareDecode = true;
            decodeDevice = provider::HardwareDecodeDevice::Nvdec;
        } else if (argument == "--retained-video")
            retainedVideo = true;
        else if (argument == "--frame-arena")
            frameArena = true;
        else if (argument == "--staged-output")
            stagedOutput = 5; // ADR 0033 section 16: ProcessSupervisor installs it as fd 5.
        else
            return 2;
    }
    if (hardwareDecode && (hardware || !retainedVideo))
        return 2; // Hardware decode is a retained, decode-only worker generation.
    if (frameArena && !retainedVideo)
        return 2; // The arena carries a retained decoder's frames only.
    if (frameArena) {
        auto opened = bloom::platform::WorkerFrameArena::open();
        auto* mapped = std::get_if<std::unique_ptr<bloom::platform::WorkerFrameArena>>(&opened);
        if (!mapped)
            return 2;
        arena = std::move(*mapped);
    }
    if (nvenc && (hardware || hardwareDecode || retainedVideo))
        return 2; // NVENC is an encode-only generation of its own.
    hardware = hardware && ffmpeg::hardwareEncodeAvailable();
    // Without a working NVIDIA driver the generation answers with the software handshake, which
    // the host does not expect, so the encode chain moves on to its next provider.
    nvenc = nvenc && ffmpeg::nvencEncodeAvailable();
    // The decode-only hardware handshake is device-neutral: the worker discovers the VA-API render
    // node or CUDA device when it opens the stream, and reports a typed refusal when none decodes
    // it.
    const auto handshake = hardwareDecode ? provider::ffmpegHardwareDecodeHandshake(decodeDevice)
                           : nvenc        ? provider::ffmpegNvencHandshake()
                                          : provider::ffmpegHandshake(hardware);
#if defined(_WIN32)
    // Streamed staged publication (ADR 0033 section 16) inherits a descriptor; Windows exports
    // return chunks instead.
    if (stagedOutput >= 0)
        return 2;
#else
    if (stagedOutput >= 0 && (retainedVideo || ::fcntl(stagedOutput, F_GETFD) < 0))
        return 2;
#endif
    ffmpeg::Encoder encoder(hardware, stagedOutput, nvenc);
    std::unique_ptr<ffmpeg::DecodeSession> decoder;
    if (retainedVideo)
        decoder = std::make_unique<ffmpeg::DecodeSession>(hardwareDecode, decodeDevice);
#endif
    try {
        std::uint64_t session = 0, sequence = 0;
        bool ready = false;
        while (true) {
            auto incoming = worker::read(retainedVideo && ready ? std::chrono::seconds(45)
                                                                : std::chrono::milliseconds{0});
            const auto* message = std::get_if<provider::Message>(&incoming);
            if (!message)
                return 3;
            if (message->sequence != sequence + 1 || (ready && message->session != session))
                return 4;
            session = message->session;
            sequence = message->sequence;
            if (message->kind == provider::MessageKind::Cancel ||
                message->kind == provider::MessageKind::Shutdown)
                return worker::write(
                           {provider::MessageKind::Ack, session, sequence, std::monostate{}})
                           ? 0
                           : 5;
            if (!ready) {
                if (message->kind != provider::MessageKind::Handshake ||
                    std::get<provider::Handshake>(message->payload) != handshake)
                    return 6;
                if (!worker::write(
                        {provider::MessageKind::Handshake, session, sequence, handshake}))
                    return 7;
                ready = true;
                continue;
            }
            if (message->kind != provider::MessageKind::Call &&
                message->kind != provider::MessageKind::EncodeBegin &&
                message->kind != provider::MessageKind::Frame &&
                message->kind != provider::MessageKind::Audio &&
                message->kind != provider::MessageKind::EncodeFinish &&
                message->kind != provider::MessageKind::EncodeRead)
                return 8;
#ifdef BLOOM_FAKE_WORKER
            if (message->kind != provider::MessageKind::Call)
                return 8;
            auto payload = fake::call(std::get<provider::CallRequest>(message->payload));
#elif defined(BLOOM_VIDEOTOOLBOX_WORKER)
            auto payload =
                message->kind == provider::MessageKind::Call
                    ? videotoolbox::call(std::get<provider::CallRequest>(message->payload))
                    : encoder.call(message->kind, message->payload);
#else
            auto payload =
                message->kind == provider::MessageKind::Call
                    ? (decoder ? decoder->call(std::get<provider::CallRequest>(message->payload))
                               : ffmpeg::call(std::get<provider::CallRequest>(message->payload),
                                              hardware))
                    : encoder.call(message->kind, message->payload);
#endif
            auto kind = provider::MessageKind::Failure;
            if (std::holds_alternative<provider::ProbeResult>(payload))
                kind = provider::MessageKind::Probe;
            if (std::holds_alternative<provider::FrameProduct>(payload))
                kind = provider::MessageKind::Frame;
            if (std::holds_alternative<provider::DemuxIndex>(payload))
                kind = provider::MessageKind::Index;
            if (std::holds_alternative<provider::AudioBlock>(payload))
                kind = provider::MessageKind::Audio;
            if (std::holds_alternative<std::monostate>(payload))
                kind = provider::MessageKind::Ack;
            if (std::holds_alternative<provider::EncodeQcV1>(payload))
                kind = provider::MessageKind::EncodeQc;
            if (std::holds_alternative<provider::EncodedChunkV1>(payload))
                kind = provider::MessageKind::EncodedChunk;
            // A frame that fits the arena crosses as one copy into shared memory; the host copies
            // it out before its next request, so one region serves every frame.
            if (arena && kind == provider::MessageKind::Frame)
                if (auto placed = provider::placeInArena(std::get<provider::FrameProduct>(payload),
                                                         arena->bytes())) {
                    payload = std::move(*placed);
                    kind = provider::MessageKind::ArenaFrame;
                }
            if (!worker::write({kind, session, sequence, std::move(payload)}))
                return 9;
#if !defined(BLOOM_FAKE_WORKER) && !defined(BLOOM_VIDEOTOOLBOX_WORKER)
            if (decoder &&
                (kind == provider::MessageKind::Frame || kind == provider::MessageKind::ArenaFrame))
                decoder->decodeAhead();
#endif
        }
    } catch (const std::exception&) {
        return 10;
    }
}
