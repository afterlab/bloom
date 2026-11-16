#pragma once

#include <string>
#include <string_view>

namespace bloom::core {

// The label this build is released under, for example "0.1.0-alpha.1". It is the one place the
// application, bloom-cli, bloom-mcp, the project manifest's generator and the Python module read
// their version from; the package files carry the same label.
[[nodiscard]] std::string_view version() noexcept;

// The compact form of a label, for a system that compares versions by its own rule (the macOS
// bundle's build version, an update list): "0.1.0-alpha.1" becomes "0.1.0a1", "-beta.N" becomes
// "bN" and "-rc.N" becomes "rcN". A label with no pre-release part, or with any other one, is
// returned unchanged. The standard comparison of those systems ranks "0.1.0-alpha.1" above
// "0.1.0" (the hyphen makes it a later patch level), and ranks "0.1.0a1" below "0.1.0", which is
// the order of a pre-release and its release.
[[nodiscard]] std::string buildVersionOf(std::string_view label);

// buildVersionOf(version()), computed once.
[[nodiscard]] std::string_view buildVersion() noexcept;

} // namespace bloom::core
