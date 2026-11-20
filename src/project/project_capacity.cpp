#include <bloom/project/project_capacity.hpp>

#include <string>

namespace bloom::project {
namespace {

// "6000000" -> "6,000,000".
[[nodiscard]] std::string grouped(const std::uint64_t value) {
    auto digits = std::to_string(value);
    for (auto position = digits.size(); position > 3; position -= 3)
        digits.insert(position - 3, 1, ',');
    return digits;
}

// "224 MiB" for whole mebibytes, otherwise the grouped bytes.
[[nodiscard]] std::string bytesText(const std::uint64_t bytes) {
    constexpr std::uint64_t mebibyte = 1ULL << 20U;
    if (bytes % mebibyte == 0)
        return grouped(bytes / mebibyte) + " MiB";
    return grouped(bytes) + " bytes";
}

} // namespace

std::string_view projectCapacityLimitName(const ProjectCapacityLimit limit) noexcept {
    switch (limit) {
    case ProjectCapacityLimit::JsonValues:
        return "projectJsonValues";
    case ProjectCapacityLimit::DocumentBytes:
        return "projectDocumentBytes";
    case ProjectCapacityLimit::ContainerEntries:
        return "projectContainerEntries";
    case ProjectCapacityLimit::StringBytes:
        return "projectStringBytes";
    }
    return "projectJsonValues";
}

std::string_view projectCapacityLimitUnit(const ProjectCapacityLimit limit) noexcept {
    switch (limit) {
    case ProjectCapacityLimit::JsonValues:
        return "values";
    case ProjectCapacityLimit::ContainerEntries:
        return "entries";
    case ProjectCapacityLimit::DocumentBytes:
    case ProjectCapacityLimit::StringBytes:
        return "bytes";
    }
    return "values";
}

std::uint64_t projectCapacityAmount(const CanonicalDocumentMeasure& measure,
                                    const ProjectCapacityLimit limit) noexcept {
    switch (limit) {
    case ProjectCapacityLimit::JsonValues:
        return measure.values;
    case ProjectCapacityLimit::DocumentBytes:
        return measure.bytes;
    case ProjectCapacityLimit::ContainerEntries:
        return measure.largestContainerEntries;
    case ProjectCapacityLimit::StringBytes:
        return measure.largestStringBytes;
    }
    return 0;
}

std::optional<ProjectCapacityExcess>
projectCapacityExcess(const CanonicalDocumentMeasure& measure,
                      const ProjectCapacityLimits& limits) noexcept {
    const auto check = [&](const ProjectCapacityLimit limit,
                           const std::uint64_t value) -> std::optional<ProjectCapacityExcess> {
        const auto actual = projectCapacityAmount(measure, limit);
        if (actual <= value)
            return std::nullopt;
        return ProjectCapacityExcess{.limit = limit, .value = value, .actual = actual};
    };
    if (auto excess = check(ProjectCapacityLimit::JsonValues, limits.jsonValues))
        return excess;
    if (auto excess = check(ProjectCapacityLimit::DocumentBytes, limits.documentBytes))
        return excess;
    if (auto excess = check(ProjectCapacityLimit::ContainerEntries, limits.containerEntries))
        return excess;
    return check(ProjectCapacityLimit::StringBytes, limits.stringBytes);
}

std::optional<ProjectCapacityExcess>
projectCapacityExcessOf(const CanonicalDocumentError error,
                        const ProjectCapacityLimits& limits) noexcept {
    switch (error) {
    case CanonicalDocumentError::ValueCountExceeded:
        return ProjectCapacityExcess{.limit = ProjectCapacityLimit::JsonValues,
                                     .value = limits.jsonValues,
                                     .actual = kCanonicalJsonMaximumValues,
                                     .atLeast = true};
    case CanonicalDocumentError::DocumentSizeExceeded:
        return ProjectCapacityExcess{.limit = ProjectCapacityLimit::DocumentBytes,
                                     .value = limits.documentBytes,
                                     .actual = kCanonicalDocumentMaximumBytes,
                                     .atLeast = true};
    case CanonicalDocumentError::ContainerEntryCountExceeded:
        return ProjectCapacityExcess{.limit = ProjectCapacityLimit::ContainerEntries,
                                     .value = limits.containerEntries,
                                     .actual = kCanonicalJsonMaximumContainerEntries,
                                     .atLeast = true};
    default:
        return std::nullopt;
    }
}

std::string projectCapacityMessage(const ProjectCapacityExcess& excess) {
    const std::string more = excess.atLeast ? " at least " : " ";
    std::string holds;
    switch (excess.limit) {
    case ProjectCapacityLimit::JsonValues:
        holds = "the project would hold" + more + grouped(excess.actual) +
                " JSON values, more than the " + grouped(excess.value) +
                " one project file can hold";
        break;
    case ProjectCapacityLimit::DocumentBytes:
        holds = "the project's document.json would be" + more + bytesText(excess.actual) +
                ", more than the " + bytesText(excess.value) + " one project file can hold";
        break;
    case ProjectCapacityLimit::ContainerEntries:
        holds = "one list in the project (such as a curve's keyframes) would hold" + more +
                grouped(excess.actual) + " entries, more than the " + grouped(excess.value) +
                " one project file can hold";
        break;
    case ProjectCapacityLimit::StringBytes:
        holds = "one text in the project would be " + bytesText(excess.actual) +
                ", more than the " + bytesText(excess.value) + " one project file can hold";
        break;
    }
    return "The edit was not applied: " + holds +
           ". Remove keyframes, layers or compositions you no longer need, or move part of the "
           "work into another project.";
}

} // namespace bloom::project
