#pragma once

#include <bloom/document/ids.hpp>

#include <QString>
#include <QWidget>

#include <functional>
#include <optional>
#include <cstdint>
#include <string_view>

namespace bloom::ui {

class CompositionSession;
namespace kit {
class KAngleDial;
class KButton;
} // namespace kit

// #512: a parameter row that carries a control of its own beside its value field, in the node card
// and in Properties alike: an angle dial before the field (as Transform's Rotation row) or a
// Randomise button after it (the Camera Shake's Seed). Keyed by the parameter's schema key so
// both surfaces build the same row.
enum class RowInline : std::uint8_t { None, AngleDial, RandomiseSeed };
[[nodiscard]] RowInline rowInlineFor(std::string_view schemaKey);

// A dial for the scalar `parameter` (degrees), bound as Rotation's: a drag or a run of keys is one
// edit, a double-click returns it to `neutral`.
[[nodiscard]] kit::KAngleDial*
makeRowAngleDial(CompositionSession& session, std::function<document::ParameterId()> parameter,
                 const QString& name, double neutral, QWidget* parent);
// Follows the parameter's value at the playhead; disabled when it is locked, driven or absent.
void refreshRowAngleDial(CompositionSession& session, kit::KAngleDial& dial,
                         std::optional<document::ParameterId> parameter, bool locked);

// A button that gives the integer `parameter` a new random value within [0, 999999] in one command.
[[nodiscard]] kit::KButton*
makeRandomiseSeedButton(CompositionSession& session,
                        std::function<std::optional<document::ParameterId>()> parameter,
                        QWidget* parent);

} // namespace bloom::ui
