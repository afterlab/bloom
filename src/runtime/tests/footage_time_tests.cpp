// Interpret Footage (#381): the frame a timed source shows (assumed and conformed rates, Loop,
// the node's start and loop), the extents layers resolve, and a conformed video's audio timing.
#include <bloom/runtime/composition_time.hpp>
#include <bloom/runtime/footage_time.hpp>

#include <bloom/runtime/layer_time_projection.hpp>

#include <iostream>

namespace {
using namespace bloom;
int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
core::RationalTime at(const std::int64_t numerator, const std::int64_t denominator) {
    return *core::RationalTime::create(numerator, denominator);
}
document::AssetRecord sequence() {
    document::AssetRecord asset;
    asset.kind = document::AssetKind::Sequence;
    asset.manifest.first = 1;
    asset.manifest.last = 10;
    return asset;
}
document::AssetRecord video() {
    document::AssetRecord asset;
    asset.kind = document::AssetKind::Video;
    asset.frames = 48;
    asset.duration = core::RationalTime::fromInteger(2);
    document::AssetVideoStream stream;
    stream.kind = 1;
    stream.framePeriod = at(1, 24);
    asset.videoStreams = {stream};
    return asset;
}
std::optional<std::uint64_t> frame(const document::AssetRecord& asset, const core::RationalTime t,
                                   const std::int64_t start = 0, const std::int64_t loop = 0) {
    return runtime::detail::footageFrameIndex(asset, t, start, loop,
                                              document::FrameRate::framesPerSecond24());
}

void testSequences() {
    auto asset = sequence();
    expect(frame(asset, at(5, 24)) == 5u, "a sequence matching the composition advances per frame");
    expect(frame(asset, at(100, 24)) == 9u, "after its end a sequence holds its last frame");
    expect(frame(asset, at(5, 24), 2) == 3u, "the node's start frame offsets the sequence");
    asset.interpretation.frameRate = document::FrameRate::create(12, 1);
    expect(frame(asset, at(5, 24)) == 2u, "an assumed 12 fps advances every second frame");
    expect(frame(asset, at(19, 24)) == 9u, "10 frames at 12 fps end at 20 composition frames");
    asset.interpretation.loopCount = 2;
    expect(frame(asset, at(21, 24)) == 0u, "Loop 2 starts the second pass");
    expect(frame(asset, at(100, 24)) == 9u, "after every pass the last frame holds");
    expect(frame(asset, at(41, 24), 0, 1) == 0u, "the node's loop wraps the whole looped length");
    asset.interpretation.frameRate = document::FrameRate::create(24000, 1001);
    expect(frame(asset, at(1, 1), 0, 1) == 3u,
           "at 23.976 fps 1 s is frame 23: with the node looping 20 frames, frame 3");
}

void testVideo() {
    auto asset = video();
    expect(frame(asset, at(12, 24)) == 12u, "the file's rate");
    asset.interpretation.frameRate = document::FrameRate::create(12, 1);
    expect(frame(asset, at(12, 24)) == 6u, "conformed to 12 fps the clip plays at half speed");
    expect(document::footageDuration(asset, document::FrameRate::framesPerSecond24()) ==
               core::RationalTime::fromInteger(4),
           "a 48 frame clip conformed to 12 fps lasts 4 s");
    const auto extent = runtime::resolveVideoStreamExtent(asset);
    expect(extent && extent->duration == core::RationalTime::fromInteger(4) &&
               extent->frameRate == document::FrameRate::create(12, 1),
           "the layer extent follows the conformed rate");
    asset.interpretation.loopCount = 3;
    expect(document::footageDuration(asset, document::FrameRate::framesPerSecond24()) ==
               core::RationalTime::fromInteger(12),
           "Loop 3 triples the length");
    expect(frame(asset, at(4 * 24 + 12, 24)) == 6u, "the second pass repeats the first");
}

void testAudio() {
    auto asset = video();
    expect(!runtime::footageAudioTiming(asset), "an uninterpreted clip's audio is untouched");
    asset.interpretation.frameRate = document::FrameRate::create(12, 1);
    asset.interpretation.loopCount = 2;
    const auto timing = runtime::footageAudioTiming(asset);
    expect(timing && timing->pass == core::RationalTime::fromInteger(4) && timing->loops == 2,
           "a conformed, looped clip carries its pass length");
    if (!timing)
        return;
    const auto start = core::RationalTime::fromInteger(1);
    const auto mapped = runtime::detail::footageAudioTime(*timing, start, at(3, 1));
    expect(mapped && std::abs(mapped->toSeconds() - 2.0) < 1e-9,
           "2 s into a half-speed clip plays its first second of audio");
    const auto second = runtime::detail::footageAudioTime(*timing, start, at(7, 1));
    expect(second && std::abs(second->toSeconds() - 2.0) < 1e-9,
           "the second pass replays the audio from the start");
    expect(!runtime::detail::footageAudioTime(*timing, start, at(10, 1)),
           "after both passes the audio is silent");
}
} // namespace

// #559: a footage layer longer than its source shows the source's last frame at the end. Preview
// and export both map layer time through wrapToExtent, so the mapped time is the proof.
void testHoldPastSourceEnd() {
    const runtime::TimeExtent extent{core::RationalTime::fromInteger(2),
                                     document::FrameRate::framesPerSecond24()};
    const auto held = runtime::wrapToExtent(at(50, 24), extent, document::LayerTimeMode::None,
                                            document::LayerTimeMode::Hold);
    expect(held && held->present && held->time == at(47, 24),
           "a layer longer than its source holds the last frame");
    const auto none = runtime::wrapToExtent(at(50, 24), extent, document::LayerTimeMode::None,
                                            document::LayerTimeMode::None);
    expect(none && !none->present, "None still shows nothing past the end");
}

// The same extent for sound. The picture holds its last frame from that frame's start; the sound
// of that frame is still the source's own, and a held picture is silent, so the audio time passes
// unchanged to the extent's end and is absent outside it (it used to stop at the last frame
// start, which played one held sample for the last frame of every movie layer).
void testAudioPlaysThroughTheLastFrame() {
    const runtime::TimeExtent extent{core::RationalTime::fromInteger(2),
                                     document::FrameRate::framesPerSecond24()};
    using Mode = document::LayerTimeMode;
    const auto inside = runtime::wrapAudioToExtent(at(95, 48), extent, Mode::Hold, Mode::Hold);
    expect(inside && inside->present && inside->time == at(95, 48),
           "inside the last frame the audio time is its own");
    const auto picture = runtime::wrapToExtent(at(95, 48), extent, Mode::Hold, Mode::Hold);
    expect(picture && picture->present && picture->time == at(47, 24),
           "the picture at that time is the last frame");
    const auto after = runtime::wrapAudioToExtent(at(2, 1), extent, Mode::Hold, Mode::Hold);
    expect(after && !after->present, "a picture held past the end is silent");
    const auto before = runtime::wrapAudioToExtent(at(-1, 24), extent, Mode::Hold, Mode::Hold);
    expect(before && !before->present, "a picture held before the start is silent");
    const auto looped = runtime::wrapAudioToExtent(at(5, 2), extent, Mode::Hold, Mode::Loop);
    expect(looped == runtime::wrapToExtent(at(5, 2), extent, Mode::Hold, Mode::Loop) && looped &&
               looped->time == at(1, 2),
           "a looped extent wraps the audio as it wraps the picture");
    // A composition source: the last frame of the nested composition sounds, then silence.
    const auto rate = document::FrameRate::framesPerSecond24();
    expect(runtime::mapCompositionTime(at(95, 48), 0.0, 1.0, extent.duration, rate, 0, true) ==
                   at(95, 48) &&
               !runtime::mapCompositionTime(at(2, 1), 0.0, 1.0, extent.duration, rate, 0, true) &&
               runtime::mapCompositionTime(at(2, 1), 0.0, 1.0, extent.duration, rate, 0) ==
                   at(47, 24),
           "a nested composition's audio plays its last frame and then stops");
}

int main() {
    testHoldPastSourceEnd();
    testAudioPlaysThroughTheLastFrame();
    testSequences();
    testVideo();
    testAudio();
    return failures == 0 ? 0 : 1;
}
