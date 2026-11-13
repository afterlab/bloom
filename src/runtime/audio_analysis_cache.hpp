#pragma once

// The prepared-analysis cache of audio_analysis.cpp, for the analyses that are computed elsewhere
// (the table analyses of audio_table_analysis.cpp) but kept under the same budget and purged with
// it.

#include <bloom/runtime/audio_analysis.hpp>

#include <memory>
#include <string>

namespace bloom::runtime::detail {

// The entry under `key`, counted as a hit, or null.
[[nodiscard]] std::shared_ptr<const AudioAnalysisResult> findAnalysisEntry(const std::string& key);
// Keeps `result` under `key`, counted as computed, and retires the least recently used entries
// beyond the budget.
void storeAnalysisEntry(const std::string& key, std::shared_ptr<const AudioAnalysisResult> result);
void countAnalysisFailure();
void countAnalysisCancelled();
// Whether `key` is cached, without counting a hit.
[[nodiscard]] bool hasAnalysisEntry(const std::string& key);

} // namespace bloom::runtime::detail
