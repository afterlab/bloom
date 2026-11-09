// Item N2-1: a node card shows the same control Properties shows for the same parameter.
// #487 adds the guard: a parameter whose values come from a closed or discoverable set (an enum, a
// probe, the project's blocks, an asset's streams) never renders as free text or a raw number;
// free text is only for the open values isOpenText() names. Every
// registered node type is added to a real composition; every parameter whose schema is a closed
// pick list (document enum, OCIO color space, LUT/audio/image/video asset, font, probe list) must
// be a KDropdown on the card AND in Properties, bools a KSwitch, colors a KColorChip. The audit
// table is printed so the docs table can be regenerated from it.
#include "channel_checklist_field.hpp"
#include "font_picker.hpp"
#include "node_editor_items.hpp"
#include "properties_registry_row.hpp"

#include <QApplication>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/parameter_enums.hpp>
#include <bloom/document/parameter_presentation.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/radio_group.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/node_editor.hpp>

#include <cstdlib>
#include <iostream>
#include <vector>

using namespace bloom;

namespace {
int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool isPickList(const document::ParameterDefinition& p) {
    return !document::parameterEnumOptions(p.schemaKey).empty() ||
           ui::isShuffleSlotSchema(p.schemaKey) ||
           p.schemaKey == document::kDataBlockParameterSchemaKey ||
           p.schemaKey == document::kVideoAudioStreamParameterSchemaKey ||
           ui::isInputColorSpaceSchema(p.schemaKey) || p.schemaKey == "bloom.ocio-file.lut" ||
           p.schemaKey == "bloom.image.asset" || p.schemaKey == "bloom.video.asset" ||
           p.schemaKey == "bloom.audio.asset" ||
           p.schemaKey == document::kTextFontParameterSchemaKey;
}

// #487: the String parameters whose value is open (the artist types it): text content, string
// operands and values, the Shuffle output layer name (it names a new layer), and the Image to
// Channel Set layer and channel names (#480: they name the layer an image becomes). Every other
// String has a closed or discoverable set and must be a dropdown or checklist. Adding a free-text
// String means adding it here, deliberately.
bool isOpenText(const std::string_view schemaKey) {
    return schemaKey == document::kTextParameterSchemaKey || schemaKey == "bloom.operand.string" ||
           schemaKey == "bloom.value.string" || schemaKey == "bloom.channel-shuffle.layer" ||
           schemaKey == "bloom.image-to-channel-set.layer" ||
           schemaKey == "bloom.image-to-channel-set.channels" ||
           // #484: names a layer of every footage leaf upstream, not of one input to list.
           schemaKey == "bloom.motion-blur.vectorLayer";
}

// #487: an Integer whose value is one of a closed or discoverable set (an enum, a choice such as a
// data block or composition reference, an audio stream, a LUT asset) is never a number field.
bool isClosedInteger(const document::ParameterDefinition& p) {
    return p.valueKind == document::ParameterValueKind::Integer &&
           (!document::parameterEnumOptions(p.schemaKey).empty() ||
            document::parameterPresentation(p).kind ==
                document::ParameterPresentationKind::Choice ||
            p.schemaKey == document::kVideoAudioStreamParameterSchemaKey ||
            p.schemaKey == "bloom.ocio-file.lut");
}

// Properties rows built by PropertiesEditor itself rather than PropertiesRegistryRow (the
// Composition source's composition dropdown).
bool hasPurposeBuiltPropertiesRow(const document::ParameterDefinition& p) {
    // The group a Node Group or Function reads is chosen in a dedicated Properties row.
    return p.schemaKey == "bloom.composition-source.composition" ||
           p.schemaKey == "bloom.node-group.group";
}

const char* controlName(QWidget* w) {
    if (qobject_cast<ui::ChannelChecklistField*>(w))
        return "checklist";
    if (qobject_cast<ui::kit::KDropdown*>(w) || qobject_cast<ui::FontField*>(w)) return "dropdown";
    if (qobject_cast<ui::kit::KSwitch*>(w)) return "switch";
    if (qobject_cast<ui::kit::KColorChip*>(w)) return "color chip";
    if (qobject_cast<ui::kit::KValueField*>(w)) return "value field";
    if (qobject_cast<ui::kit::KLineEdit*>(w)) return "text field";
    if (qobject_cast<ui::kit::KRadioGroup*>(w)) return "segmented";
    return w ? "other" : "none";
}
const char* kindName(const document::ParameterValueKind k) {
    switch (k) {
    case document::ParameterValueKind::Boolean: return "bool";
    case document::ParameterValueKind::Integer: return "int";
    case document::ParameterValueKind::Float64: return "float";
    case document::ParameterValueKind::Vec2d: return "vec2";
    case document::ParameterValueKind::Vec3d: return "vec3";
    case document::ParameterValueKind::Color4d: return "color";
    case document::ParameterValueKind::String: return "string";
    case document::ParameterValueKind::Path: return "path";
    case document::ParameterValueKind::Gradient: return "gradient";
    }
    return "?";
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // Capture mode (a review tool, not the test): node_field_controls_test <dir> <1|1.5>.
    const bool capture = argc >= 3;
    if (capture)
        qputenv("QT_SCALE_FACTOR", argv[2]);
    QApplication app(argc, argv);
    ui::kit::installKinetikTheme(app);
    auto project = document::makeNewProject("Fields", "Main", core::RationalTime::fromInteger(10));
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, project.initialCompositionId);
    ui::NodeGraphEditor editor(session);
    editor.resize(1400, 900);
    editor.show();
    QCoreApplication::processEvents();

    std::vector<std::pair<std::string, document::NodeId>> added;
    const bool print = std::getenv("BLOOM_PRINT_FIELD_AUDIT") != nullptr;
    int audited = 0;
    double x = 0.0;
    for (const auto& definition : document::builtInNodeDefinitions().definitions()) {
        commands::Transaction transaction("Add", session.snapshot().revision());
        transaction.emplace<commands::AddNode>(session.compositionId(), definition.key.typeId,
                                               document::Vec2d{x, 0.0});
        x += 400.0;
        const auto result = session.executeNodeTransaction(std::move(transaction));
        const auto id = result.outputId<document::NodeId>(commands::kAddNodeOutput);
        if (!id)
            continue; // a structural singleton that already exists
        QCoreApplication::processEvents();
        const auto* node = session.composition()->graph().findNode(*id);
        if (node == nullptr)
            continue;
        added.emplace_back(definition.key.typeId, *id);
        for (const auto& declared : definition.parameters) {
            const auto binding = std::ranges::find(node->parameters, declared.role,
                                                   &document::ParameterBinding::role);
            if (binding == node->parameters.end())
                continue;
            const auto role = QString::fromStdString(declared.role);
            const auto controls = editor.graphScene()->nodeControlsForRoleForTest(*id, role);
            QWidget* card = controls.isEmpty() ? nullptr : controls.front();
            ui::PropertiesRegistryRow row(session, *id, binding->parameterId, declared, nullptr);
            QWidget* props = row.findChild<ui::ChannelChecklistField*>();
            if (props == nullptr)
                props = row.findChild<ui::kit::KDropdown*>();
            if (props == nullptr)
                props = row.findChild<ui::kit::KRadioGroup*>();
            if (props == nullptr)
                props = row.findChild<ui::kit::KSwitch*>();
            if (props == nullptr)
                props = row.findChild<ui::kit::KColorChip*>();
            if (props == nullptr)
                props = row.findChild<ui::kit::KLineEdit*>();
            if (props == nullptr)
                props = row.findChild<ui::kit::KValueField*>();
            ++audited;
            const std::string label = definition.key.typeId + "." + declared.role;
            if (print)
                std::cout << "AUDIT|" << definition.key.typeId << '|' << declared.role << '|'
                          << declared.schemaKey << '|' << kindName(declared.valueKind) << '|'
                          << controlName(card) << '|' << controlName(props) << '\n';
            const bool hidden =
                ui::propertiesRowVisibility(declared.role, declared.schemaKey) ==
                ui::PropertiesRowVisibility::Hidden;
            // #487 guard: a closed or discoverable value set never renders as free text or a raw
            // number, on the card or in Properties (where a hidden row has a purpose-built one).
            const bool propsShown = !hidden && !hasPurposeBuiltPropertiesRow(declared);
            const auto freeText = [](QWidget* w) {
                return qobject_cast<ui::kit::KLineEdit*>(w) != nullptr;
            };
            const auto rawNumber = [](QWidget* w) {
                return qobject_cast<ui::kit::KValueField*>(w) != nullptr;
            };
            if (declared.valueKind == document::ParameterValueKind::String &&
                !isOpenText(declared.schemaKey)) {
                expect(!freeText(card), "card shows no free text for the closed " + label);
                expect(!propsShown || !freeText(props),
                       "Properties shows no free text for the closed " + label);
            }
            if (isClosedInteger(declared)) {
                expect(!rawNumber(card), "card shows no number field for the closed " + label);
                expect(!propsShown || !rawNumber(props),
                       "Properties shows no number field for the closed " + label);
            }
            if (card == nullptr)
                continue; // not drawn as an editable card row (special rows, hidden, path)
            if (isPickList(declared) && !hidden) {
                // The font is the font field: a dropdown that opens the picker (#489).
                expect(qobject_cast<ui::kit::KDropdown*>(card) != nullptr ||
                           qobject_cast<ui::FontField*>(card) != nullptr,
                       "card shows a dropdown for " + label);
                expect(qobject_cast<ui::kit::KDropdown*>(props) != nullptr ||
                           qobject_cast<ui::kit::KRadioGroup*>(props) != nullptr ||
                           qobject_cast<ui::FontField*>(props) != nullptr,
                       "Properties shows a dropdown for " + label);
            }
            if (declared.valueKind == document::ParameterValueKind::Boolean)
                expect(qobject_cast<ui::kit::KSwitch*>(card) != nullptr,
                       "card shows a switch for " + label);
            if (declared.valueKind == document::ParameterValueKind::Color4d)
                expect(qobject_cast<ui::kit::KColorChip*>(card) != nullptr ||
                           qobject_cast<ui::kit::KValueField*>(card) != nullptr,
                       "card shows a color control for " + label);
        }
    }
    if (capture) {
        const QString suffix = QString::fromLatin1(argv[2]).remove(QLatin1Char('.'));
        editor.resize(520, 560);
        editor.graphView()->zoomToActualSize();
        for (const auto& [type, id] : added) {
            for (const char* wanted :
                 {"bloom.ocio-colour-space-transform", "bloom.ocio-file-transform",
                  "bloom.gaussian-blur", "bloom.fit", "bloom.text-source", "bloom.image-source",
                  "bloom.linear-gradient", "bloom.channel-shuffle", "bloom.video-source"}) {
                if (type != wanted)
                    continue;
                if (auto* item = editor.graphScene()->findNodeItem(id))
                    editor.graphView()->centerOn(item);
                QCoreApplication::processEvents();
                const auto file = QString::fromStdString(type).replace(QLatin1Char('.'), '-') +
                                  QStringLiteral("-dpr") + suffix + QStringLiteral(".png");
                editor.grab().save(QString::fromLocal8Bit(argv[1]) + QLatin1Char('/') + file);
            }
        }
    }
    expect(audited > 50, "the audit reached the registry's parameters");
    std::cerr << "audited parameters: " << audited << '\n';
    return failures == 0 ? 0 : 1;
}
