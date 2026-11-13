#pragma once

// A thumbnail for a data-only image (no colour layer): the Cryptomatte ID colours of its first
// Cryptomatte type, or its first data channel as grey. Worker-only; decodes two channels at most.

#include <QImage>
#include <bloom/media/image.hpp>

#include <filesystem>
#include <functional>
#include <string>

namespace bloom::ui::detail {

[[nodiscard]] QImage dataChannelThumbnail(const media::ImageSourceRevision& revision,
                                          const std::string& part,
                                          const std::function<bool()>& cancel);

} // namespace bloom::ui::detail
