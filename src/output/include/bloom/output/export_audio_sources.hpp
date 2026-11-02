#pragma once

#include <bloom/document/document.hpp>
#include <bloom/media/provider/contract.hpp>
#include <bloom/runtime/compiled_plan.hpp>

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

// True when the export audio path decodes this probed codec spelling.
[[nodiscard]] bool exportAudioPathDecodesV1(const std::string& codec);

} // namespace bloom::output
