#pragma once

#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_json_writer.hpp>
#include <bloom/project/project_io_memory.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// #748: a project Bloom lets you build always saves. Every edit is measured with the canonical
// encoder's own counting pass (measureCanonicalDocument) and one that would take the project past
// these limits is refused at the edit, never at the save. They sit below the format's read limits
// (docs/architecture/project-format.md) by a reserve that covers what a save adds beyond the
// document it measures (sequence digests, the manifest) and keeps the save's peak memory -- about
// 300 bytes per JSON value for a keyframe-dense project -- inside the default per-operation budget,
// so a project at these limits saves and reopens.
namespace bloom::project {

struct ProjectCapacityLimits final {
    std::uint64_t jsonValues = 6'000'000;
    std::uint64_t documentBytes = 224ULL << 20U;
    std::uint64_t containerEntries = 999'000;
    std::uint64_t stringBytes = (64ULL << 20U) - (64ULL << 10U);
};

// The most one JSON value of a keyframe-dense project charges at the peak of its save, and the
// fixed reservations of an otherwise empty save (save_archive_tests.cpp measures both).
inline constexpr std::uint64_t kProjectCapacitySaveBytesPerJsonValue = 300;
inline constexpr std::uint64_t kProjectCapacityFixedSaveBytes = 12ULL << 20U;

static_assert(ProjectCapacityLimits{}.jsonValues < kCanonicalJsonMaximumValues);
static_assert(ProjectCapacityLimits{}.documentBytes < kCanonicalDocumentMaximumBytes);
static_assert(ProjectCapacityLimits{}.containerEntries < kCanonicalJsonMaximumContainerEntries);
static_assert(ProjectCapacityLimits{}.stringBytes < (64ULL << 20U));
static_assert(ProjectCapacityLimits{}.jsonValues * kProjectCapacitySaveBytesPerJsonValue +
                  kProjectCapacityFixedSaveBytes <=
              kDefaultProjectIoOperationResidentBytes);

enum class ProjectCapacityLimit : std::uint8_t {
    JsonValues,
    DocumentBytes,
    ContainerEntries,
    StringBytes,
};

// A limit a measured project passes: the limit's value and what the project holds (at least, when
// it passed the format's own limit and the encoder stopped counting).
struct ProjectCapacityExcess final {
    ProjectCapacityLimit limit = ProjectCapacityLimit::JsonValues;
    std::uint64_t value = 0;
    std::uint64_t actual = 0;
    bool atLeast = false;
};

// Stable names ("projectJsonValues", "projectDocumentBytes", "projectContainerEntries",
// "projectStringBytes") and units ("values", "bytes", "entries", "bytes").
[[nodiscard]] std::string_view projectCapacityLimitName(ProjectCapacityLimit limit) noexcept;
[[nodiscard]] std::string_view projectCapacityLimitUnit(ProjectCapacityLimit limit) noexcept;
[[nodiscard]] std::uint64_t projectCapacityAmount(const CanonicalDocumentMeasure& measure,
                                                  ProjectCapacityLimit limit) noexcept;

// The first limit a measure passes, or nullopt.
[[nodiscard]] std::optional<ProjectCapacityExcess>
projectCapacityExcess(const CanonicalDocumentMeasure& measure,
                      const ProjectCapacityLimits& limits = {}) noexcept;

// The limit an encoder refusal at the format's own limits passed, for a measure that stopped
// there; nullopt for an error that is not about size.
[[nodiscard]] std::optional<ProjectCapacityExcess>
projectCapacityExcessOf(CanonicalDocumentError error,
                        const ProjectCapacityLimits& limits = {}) noexcept;

// One sentence for the artist and the agent: "The edit was not applied: the project would hold
// 6,000,412 JSON values, more than the 6,000,000 one project file can hold. Remove keyframes,
// layers or compositions you no longer need, or move part of the work into another project."
[[nodiscard]] std::string projectCapacityMessage(const ProjectCapacityExcess& excess);

} // namespace bloom::project
