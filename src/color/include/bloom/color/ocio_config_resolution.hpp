#pragma once

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/core/sha256.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Resolution of any OCIO config a project can reference (docs/architecture/color-management.md,
// "Durable OCIO Configuration Identity" and "Supervised OCIO Execution"). The built-ins resolve
// in-process through ocio_builtin_registry.hpp. A project-relative archive, an external archive
// and an external loose config are read, structurally checked and hashed here, then parsed, built
// and applied only in the supervised bloom-color-worker helper; the product is the same
// ResolvedBloomNeutralConfig, whose processors are tokens of that helper. A target that cannot
// confine the helper reports LocatorKindRequiresHelper with HelperUnavailable and never parses an
// external config in-process.

namespace bloom::color {

struct OcioConfigContextVariable final {
    std::string name;
    std::string value;
    friend bool operator==(const OcioConfigContextVariable&,
                           const OcioConfigContextVariable&) = default;
};

// Where a config is and how it is opened. `value` is the built-in URI, or for the three file
// kinds a local absolute path or file URI: of the .ocioz archive, or of a loose root's
// config.ocio. A project-relative archive carries its path already joined to the project's
// directory. `context` is the project's OCIO context variables, sorted by name.
struct OcioConfigLocation final {
    OcioConfigLocatorKind kind = OcioConfigLocatorKind::BloomBuiltIn;
    std::string value;
    std::vector<OcioConfigContextVariable> context;
    friend bool operator==(const OcioConfigLocation&, const OcioConfigLocation&) = default;
};

// The one-line text an evaluation color intent carries for a location, and its inverse. A
// built-in with no context variables is its plain URI, so existing intents are unchanged. Text
// that names no file kind parses as a built-in and resolves Missing unless it is one.
[[nodiscard]] std::string ocioConfigLocationText(const OcioConfigLocation& location);
[[nodiscard]] OcioConfigLocation parseOcioConfigLocation(std::string_view text);

// Resolves a location against its expected content revision and proves the working space, as
// resolveOcioBuiltIn does for a built-in. For the file kinds:
//   Missing                    the file is absent, not a regular file or not a local absolute path
//   Changed                    the content revision differs; recomputedRevision() is the current
//   one Invalid                    OCIO rejected the config, or it exceeds the resource limits
//   MissingResource            a file the config references is not in the archive or loose root
//   LocatorKindRequiresHelper  the helper could not serve the request; helperError() says why
// A Ready file config is kept, with its helper, while the file's size, time and identity (for a
// loose root: of every candidate file) are unchanged, so the same config is not parsed again for
// every frame. Blocking: call it where a processor build may block.
[[nodiscard]] OcioBuiltInResolutionResult
resolveOcioConfig(const OcioConfigLocation& location, const core::Sha256Digest& expectedRevision,
                  std::string_view workingColorSpaceId = {}) noexcept;
[[nodiscard]] OcioBuiltInResolutionResult
resolveOcioConfig(std::string_view locationText, const core::Sha256Digest& expectedRevision,
                  std::string_view workingColorSpaceId = {}) noexcept;

// The current content revision of a location, for a caller that proposes a durable reference.
// Empty when the config cannot be read or, for a loose config, when the helper did not accept it.
[[nodiscard]] std::optional<core::Sha256Digest>
ocioConfigContentRevision(const OcioConfigLocation& location) noexcept;

// Ends every kept helper. Configs and processors already handed out keep theirs until released.
void releaseExternalOcioConfigs() noexcept;

} // namespace bloom::color
