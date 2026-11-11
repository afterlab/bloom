// Compile-and-value checks for the proposed export queue contract headers (ADR 0033). The
// service, catalogue and subscription are declared but not implemented yet, so this test only
// exercises value types and the constexpr helpers; it must not ODR-use an undefined member.

#include <bloom/document/export_queue.hpp>
#include <bloom/host/export_presets.hpp>
#include <bloom/host/export_queue.hpp>
#include <bloom/host/export_verification.hpp>

#include <iostream>
#include <type_traits>

namespace {

using namespace bloom;

static_assert(!std::is_copy_constructible_v<host::ExportQueueService>);
static_assert(!std::is_copy_constructible_v<host::ExportQueueSubscription>);
static_assert(std::is_nothrow_move_constructible_v<host::ExportQueueSubscription>);
static_assert(!std::is_copy_constructible_v<host::ExportPresetCatalogue>);

// The lifecycle helpers partition every state exactly once into Ready, active and terminal.
static_assert(!host::isActive(host::ExportJobState::Ready));
static_assert(!host::isTerminal(host::ExportJobState::Ready));
static_assert(host::isActive(host::ExportJobState::Waiting));
static_assert(!host::isRunning(host::ExportJobState::Waiting));
static_assert(host::isRunning(host::ExportJobState::Paused));
static_assert(host::isActive(host::ExportJobState::Stopping));
static_assert(!host::isActive(host::ExportJobState::Done));
static_assert(host::isTerminal(host::ExportJobState::Stopped));

// Contract table pins: session commands never touch project truth, edits always do.
static_assert(host::exportCommandKind(host::ExportCommand::Start) ==
              host::ExportCommandKind::Session);
static_assert(host::exportCommandKind(host::ExportCommand::Retry) ==
              host::ExportCommandKind::Session);
static_assert(host::exportCommandKind(host::ExportCommand::RevealOutput) ==
              host::ExportCommandKind::Session);
static_assert(host::exportCommandKind(host::ExportCommand::AddJob) ==
              host::ExportCommandKind::ProjectTruth);
static_assert(host::exportCommandKind(host::ExportCommand::RemoveFinished) ==
              host::ExportCommandKind::ProjectTruth);
static_assert(host::exportCommandKind(host::ExportCommand::EditOutput) ==
              host::ExportCommandKind::ProjectTruth);
static_assert(host::exportCommandKind(host::ExportCommand::ConfirmReplace) ==
              host::ExportCommandKind::Session);
static_assert(static_cast<std::size_t>(host::ExportCommand::ConfirmReplace) + 1 ==
              host::kExportCommandCount);

int expect(const bool condition, const char* message) {
    if (condition)
        return 0;
    std::cerr << message << '\n';
    return 1;
}

} // namespace

int main() {
    int failures = 0;

    document::ExportJob job;
    job.id = document::ExportJobId::fromRaw(7);
    job.composition = document::CompositionId::fromRaw(3);
    document::ExportOutput handoff;
    handoff.id = document::ExportOutputId::fromRaw(1);
    handoff.presetId = "builtin:vfx-handoff-exr";
    handoff.settings.format = document::ExportFormat::OpenExr;
    handoff.settings.compression = "piz";
    handoff.settings.outputColorSpaceId = "ACES2065-1";
    handoff.settings.bakeLook = false;
    handoff.settings.naming.startFrame = 1001;
    job.outputs.push_back(handoff);
    document::ExportQueue queue;
    queue.jobs.push_back(job);
    failures += expect(queue.jobs.front().render.range.kind == document::ExportRangeKind::WorkArea,
                       "a new job renders the work area by default");
    failures += expect(document::ExportOutputSettings{}.format == document::ExportFormat::H264Mp4,
                       "default output settings are the MP4/H.264 video default");
    failures += expect(queue.jobs.front().outputs.front().destination.overwrite ==
                           document::ExportOverwritePolicy::AskBeforeReplacing,
                       "a new output asks before replacing an existing file");
    failures += expect(!queue.jobs.front().outputs.front().importIntoAssets,
                       "import into Assets is off by default");
    failures += expect(!queue.jobs.front().labelColor.has_value(), "a new job has no label");

    host::ExportPreflightIssue exists;
    exists.code = host::ExportPreflightCode::DestinationExists;
    exists.severity = host::ExportPreflightSeverity::NeedsConfirmation;
    failures += expect(exists.target == host::ExportPreflightTarget::Destination,
                       "a preflight issue names the field it is about");

    host::ExportOutputView outputView;
    failures += expect(!outputView.estimatedBytes && !outputView.freeBytesAtDestination &&
                           outputView.import.status == host::ExportAssetImportStatus::NotRequested,
                       "size facts may be absent and imports start NotRequested");

    host::ExportJobView view;
    view.id = job.id;
    view.revision = 1;
    view.record = job;
    failures += expect(view.state == host::ExportJobState::Ready, "a job view starts Ready");
    failures += expect(!view.commands[host::ExportCommand::Pause].available &&
                           view.commands[host::ExportCommand::Pause].code ==
                               host::ExportUnavailableCode::NotApplicable,
                       "a default availability set refuses with NotApplicable");

    host::ExportQueueEvent event;
    event.sequence = 1;
    event.revision = 2;
    event.payload = host::ExportQueueDrained{};
    failures += expect(std::holds_alternative<host::ExportQueueDrained>(event.payload),
                       "queue drained is a typed terminal event");

    host::ExportCheckResult check;
    check.check = host::ExportCheck::ColourTags;
    check.outcome = host::ExportCheckOutcome::Passed;
    check.code = "export.check.colour-tags.match";
    check.expected = host::ExportColourTags{1, 1, 1, false};
    check.observed = host::ExportColourTags{1, 1, 1, false};
    failures += expect(check.expected == check.observed, "color tags compare structurally");

    // #465: Timing and Audio compare the reopened stream with the request. 48 frames at
    // 30000/1001 with 48 kHz audio: floor(48 * 1001 / 30000 * 48000) = 76876 samples.
    using host::ExportCheckOutcome;
    const host::ExportStreamRequest movie{48, {30000, 1001}, 76876, 48000};
    const host::ExportStreamObservation exact{48, 76876, {48048, 30000}};
    failures += expect(host::exportTimingCheck(movie, exact).outcome == ExportCheckOutcome::Passed,
                       "a movie whose duration and audio match the request passes Timing");
    failures += expect(host::exportAudioCheck(movie, exact).outcome == ExportCheckOutcome::Passed,
                       "a movie whose audio sample count matches the request passes Audio");
    auto longer = exact;
    longer.duration = {49049, 30000};
    failures += expect(host::exportTimingCheck(movie, longer).outcome == ExportCheckOutcome::Failed,
                       "a reopened duration other than the requested one fails Timing");
    auto shortAudio = exact;
    shortAudio.audioSamples = 76000;
    failures +=
        expect(host::exportTimingCheck(movie, shortAudio).outcome == ExportCheckOutcome::Failed &&
                   host::exportTimingCheck(movie, shortAudio).code ==
                       "export.check.timing.audio-duration-mismatch",
               "audio that ends before the video fails Timing");
    failures +=
        expect(host::exportAudioCheck(movie, shortAudio).outcome == ExportCheckOutcome::Failed,
               "a sample count other than the requested one fails Audio");
    failures += expect(host::exportTimingCheck(movie, {48, 76876, {0, 1}}).outcome ==
                           ExportCheckOutcome::Failed,
                       "an empty duration fails Timing");
    const host::ExportStreamRequest silent{48, {24, 1}, 0, 48000};
    failures += expect(host::exportTimingCheck(silent, {48, 0, {2, 1}}).outcome ==
                               ExportCheckOutcome::Passed &&
                           host::exportAudioCheck(silent, {48, 0, {2, 1}}).outcome ==
                               ExportCheckOutcome::NotApplicable,
                       "a silent movie passes Timing and does not apply Audio");
    failures += expect(host::exportAudioCheck(silent, {48, 96000, {2, 1}}).outcome ==
                           ExportCheckOutcome::Failed,
                       "audio nobody asked for fails Audio");
    const host::ExportStreamRequest wav{0, {24, 1}, 96000, 48000};
    failures += expect(
        host::exportTimingCheck(wav, {0, 96000, {2, 1}}).outcome == ExportCheckOutcome::Passed &&
            host::exportTimingCheck(wav, {0, 96000, {3, 1}}).outcome == ExportCheckOutcome::Failed,
        "a WAV's duration is its sample count over its sample rate");

    // #465: HEVC Main 10 goes to a hardware encoder only when its probe opened Main 10.
    document::ExportOutputSettings hevc10;
    hevc10.format = document::ExportFormat::HevcMp4;
    hevc10.codecProfile = "main10";
    auto hevc8 = hevc10;
    hevc8.codecProfile = "main";
    host::ExportEncoderStatus eightBit;
    eightBit.nvencHevc = true;
    eightBit.vaapiHevc = true;
    using Route = host::ExportHardwareRoute;
    failures += expect(host::exportHardwareRoute(hevc8, eightBit) == Route{true, true},
                       "HEVC Main takes NVENC and VA-API when both open 8-bit HEVC");
    failures += expect(host::exportHardwareRoute(hevc10, eightBit) == Route{false, false},
                       "HEVC Main 10 takes neither when only 8-bit HEVC opened");
    auto tenBit = eightBit;
    tenBit.nvencHevcMain10 = true;
    failures += expect(host::exportHardwareRoute(hevc10, tenBit) == Route{true, false},
                       "HEVC Main 10 takes NVENC when NVENC opened Main 10");
    tenBit.nvencHevcMain10 = false;
    tenBit.vaapiHevcMain10 = true;
    failures += expect(host::exportHardwareRoute(hevc10, tenBit) == Route{false, true},
                       "HEVC Main 10 takes VA-API when VA-API opened Main 10");
    auto forced = hevc10;
    forced.encoder = document::ExportEncoderPreference::Hardware;
    using Platform = media::provider::PlatformEncoderV1;
    failures += expect(host::exportHardwareRoute(forced, eightBit) ==
                           Route{false, true, Platform::Required},
                       "Hardware still asks VA-API for Main 10 and falls back if refused");
    auto software = hevc8;
    software.encoder = document::ExportEncoderPreference::Software;
    failures += expect(host::exportHardwareRoute(software, tenBit) ==
                           Route{false, false, Platform::Skipped},
                       "Software takes no hardware encoder");
    // The platform's own encoder (macOS): ProRes requires it unless the output asks for the
    // software encoder, so FFmpeg is never substituted for it silently; H.264 prefers it.
    document::ExportOutputSettings prores;
    prores.format = document::ExportFormat::ProResMov;
    failures += expect(host::exportHardwareRoute(prores, eightBit).platformEncoder ==
                           Platform::Required,
                       "ProRes requires the platform encoder by default");
    prores.encoder = document::ExportEncoderPreference::Software;
    failures += expect(host::exportHardwareRoute(prores, eightBit).platformEncoder ==
                           Platform::Skipped,
                       "ProRes with the software encoder skips the platform encoder");
    document::ExportOutputSettings h264;
    h264.format = document::ExportFormat::H264Mp4;
    failures += expect(host::exportHardwareRoute(h264, eightBit).platformEncoder ==
                           Platform::Preferred,
                       "H.264 prefers the platform encoder and may fall back");

    host::ExportPreset preset;
    preset.id = std::string(host::kExportDefaultVideoPresetId);
    failures +=
        expect(preset.category == host::ExportPresetCategory::Video &&
                   host::ExportPresetCategory::Video < host::ExportPresetCategory::VfxSequences,
               "the default preset is video and video sorts before sequences");
    failures +=
        expect(!preset.platformAvailability.available &&
                   preset.platformAvailability.code == host::ExportAvailabilityCode::Probing,
               "an unprobed preset reports Probing, not unavailable");

    return failures == 0 ? 0 : 1;
}
