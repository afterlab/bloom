#pragma once

#include <bloom/document/document.hpp>
#include <bloom/media/provider/contract.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/platform/source_store.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace bloom::output {

// One audio stream a composition's mix reads: the asset and its 0-based audio stream.
struct ExportAudioSourceV1 final {
    document::AssetId asset;
    std::uint32_t audioStream = 0;
    friend bool operator==(const ExportAudioSourceV1&, const ExportAudioSourceV1&) = default;
};

// Every distinct audio stream the plan's mix reads, nested compositions included, in first-use
// order.
[[nodiscard]] std::vector<ExportAudioSourceV1>
exportAudioSourcesV1(const runtime::CompiledCompositionPlan& plan);

// What the source-aware audio preflight learned about the sources it accepted.
struct ExportAudioSourceFactsV1 final {
    // True when a source carries PCM deeper than 16 bits or floating-point samples, so a PCM
    // container should keep 24 bits instead of 16.
    bool deeperThan16Bit = false;
};

// Source-aware export audio preflight (#428). Probes every audio stream the mix reads through the
// media provider and refuses, before rendering or opening an encoder, a source the export audio
// path cannot decode: the bundled FFmpeg worker must declare an AudioDecode capability for the
// stream's exact codec, because a decoder pins its declaration's codec exactly. The refusal is a
// plain sentence that names the stage and the input, for example "Audio asset 4 (adpcm_ms) is not
// accepted by the export audio path."
[[nodiscard]] std::variant<ExportAudioSourceFactsV1, media::provider::Unavailable>
preflightExportAudioSourcesV1(const document::Snapshot& snapshot,
                              const runtime::CompiledCompositionPlan& plan,
                              const std::filesystem::path& assetBaseDirectory,
                              const std::function<bool()>& cancel);

// The asset's "Keep local copies" choice as the source store's override: the export audio
// sessions read a movie's sound through the process's source store with it (#732).
[[nodiscard]] platform::LocalCopyOverride
exportAudioLocalCopiesV1(const document::AssetInterpretation& interpretation) noexcept;

// #765: whether a clip of the export's mix plays any of its source in the programme span
// [from, to) at `rate` output frames per second. The export opens and holds a source only for the
// spans its clips play, so what an export decodes follows the range, not the number of layers.
// The answer may be true for a span the clip leaves silent and is never false for one it plays:
// each output frame is mapped through the clip's time mappings as the mixer maps it, with a
// margin of two frames for the mixer's rounding. An audio asset's recorded length (with half a
// second to spare) ends the clip once its sound has played; `asset` may be null.
[[nodiscard]] bool exportAudioClipSoundsV1(const runtime::AudioClipDescription& clip,
                                           const document::AssetRecord* asset,
                                           core::RationalTime from, core::RationalTime to,
                                           std::uint32_t rate);

// The processing root's host memory when an audio admission was refused.
struct ExportAudioHostMemoryV1 final {
    std::uint64_t freeBytes = 0;
    std::uint64_t usableBytes = 0;
};

// #765: a refused admission of the memory one audio source needs for the export.
struct ExportAudioAdmissionRefusalV1 final {
    // What was refused, such as "Audio backing admission refused".
    std::string refused;
    document::AssetId asset;
    std::uint64_t bytes = 0;
    // What the bytes are for, such as "its decoded audio window".
    std::string purpose;
    ExportResourceAdmissionStatusV1 status = ExportResourceAdmissionStatusV1::Reserved;
    // The export's own memory allowance, when it has one.
    std::optional<std::uint64_t> allowance;
    // The allowance shared by running exports that the claim counts against, when it does.
    std::optional<std::uint64_t> serviceAllowance;
    // The processing root's measured host memory, when the export runs under one.
    std::optional<ExportAudioHostMemoryV1> hostMemory;
    // The export's audio sources held decoded at the time.
    std::size_t decodedSources = 0;
};

// The refusal as one sentence that names the resource, the amount and the limit that refused it:
// "Audio backing admission refused: audio asset 7 needs 393240 bytes of memory for its decoded
// audio window, but there is not enough free memory: the processing memory has 1024 of 4294967296
// bytes free (3 audio sources of this export were decoded at the time)."
[[nodiscard]] std::string
describeExportAudioAdmissionRefusalV1(const ExportAudioAdmissionRefusalV1&);

// True when the export audio path decodes this probed codec spelling.
[[nodiscard]] bool exportAudioPathDecodesV1(const std::string& codec);

// True when the export audio path decodes this probed codec spelling.
[[nodiscard]] bool exportAudioPathDecodesV1(const std::string& codec);

} // namespace bloom::output