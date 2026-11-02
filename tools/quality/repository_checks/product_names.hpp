#pragma once
#include "repository_checks.hpp"
#include <string_view>
namespace bloom::quality {
// Public copy names no commercial product: README.md and docs/ in full, and the comments and
// string literals of src/**/*.{cpp,hpp}. Copy describes the workflow instead ("a layer-based
// compositor", "a render queue"); format and standard names (ProRes, OpenEXR, OCIO, ACES) are
// fine. Third-party sources and the owner-edited node catalogue are outside the rule.
[[nodiscard]] std::vector<RepositoryFinding>
productNameViolations(const std::filesystem::path& path, std::string_view source);
[[nodiscard]] std::vector<RepositoryFinding>
scanProductNames(const std::filesystem::path& root, std::span<const std::filesystem::path> files);
} // namespace bloom::quality
