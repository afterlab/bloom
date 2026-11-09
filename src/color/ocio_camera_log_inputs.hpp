#pragma once

// Private to bloom_color_ocio (#381 Interpret Footage): camera log input spaces Bloom synthesizes
// into a resolved pinned config that lacks them, from OCIO's own camera built-ins, as it does the
// Rec.2100 HLG space.

#include "ocio_internal.hpp"

#include <string>
#include <string_view>

namespace bloom::color::detail {

// The config with every camera log space it does not already name added (S-Log3, LogC3, LogC4,
// V-Log, Log3G10, REDLogFilm, C-Log2, C-Log3, Apple Log). Each is OCIO's "<camera>_to_ACES2065-1"
// built-in, taken from a raw OCIO 2.4 config so the pinned configs keep their versions, then
// ACES2065-1 to the config reference (Bradford to XYZ D65 and Rec.709 for Bloom Neutral; nothing
// for the ACES configs, whose reference is ACES2065-1). The copy is validated.
[[nodiscard]] OCIO::ConstConfigRcPtr withBloomCameraLogInputs(const OCIO::ConstConfigRcPtr& config,
                                                              bool isNeutral);

// The config with kBloomCineonLogColorSpaceId added when it does not name one (#426): Cineon
// printing density to the config reference, with Rec.709 primaries assumed. The copy is validated.
[[nodiscard]] OCIO::ConstConfigRcPtr withBloomCineonInput(const OCIO::ConstConfigRcPtr& config,
                                                          bool isNeutral);

// synthesizedColorSpaceIdentity() for a camera log space; empty for any other id.
[[nodiscard]] std::string cameraLogColorSpaceIdentity(std::string_view colorSpaceId);

} // namespace bloom::color::detail
