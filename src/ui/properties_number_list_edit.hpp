#pragma once

#include <bloom/document/parameter.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/inline_edit.hpp>
#include <bloom/ui/kit/text_field.hpp>

#include <QCoreApplication>
#include <QLocale>
#include <QObject>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <cmath>
#include <optional>
#include <variant>

namespace bloom::ui::properties {

// The one number-list editor (#627, docs/architecture/number-list.md). A list is typed as its
// numbers separated by commas or spaces, in the C locale so a project reads the same everywhere.
// The text shown for a list is the shortest text that reads back as the same doubles.

[[nodiscard]] inline QString numberListText(const document::NumberListValue& list) {
    QStringList items;
    items.reserve(static_cast<qsizetype>(list.values.size()));
    for (const double value : list.values)
        items.push_back(QLocale::c().toString(value, 'g', QLocale::FloatingPointShortest));
    return items.join(QStringLiteral(", "));
}

// The list `text` spells, or nullopt with the plain reason in `reason`. Empty text is the empty
// list.
[[nodiscard]] inline std::optional<document::NumberListValue>
parseNumberList(const QString& text, QString* reason = nullptr) {
    const auto refuse = [reason](QString why) -> std::optional<document::NumberListValue> {
        if (reason != nullptr)
            *reason = std::move(why);
        return std::nullopt;
    };
    static const QRegularExpression separators(QStringLiteral("[,;\\s]+"));
    const auto items = text.split(separators, Qt::SkipEmptyParts);
    if (static_cast<std::size_t>(items.size()) > document::kMaximumNumberListItems)
        return refuse(QCoreApplication::translate("number_list",
                                                  "A list holds at most %1 numbers.")
                          .arg(document::kMaximumNumberListItems));
    document::NumberListValue list;
    list.values.reserve(static_cast<std::size_t>(items.size()));
    for (const auto& item : items) {
        bool ok = false;
        const double value = QLocale::c().toDouble(item, &ok);
        if (!ok || !std::isfinite(value))
            return refuse(
                QCoreApplication::translate("number_list", "\"%1\" is not a number.").arg(item));
        list.values.push_back(value);
    }
    return list;
}

// Binds a kit text field to a number-list parameter. The kit inline-edit policy ends the edit
// (Return, Escape, Tab and focus loss all commit); text that is not a list is refused with its
// reason and never written, and an edit that ends refused shows the document's list again. One
// commit is one transaction, and text that spells the list the document already holds commits
// nothing. After either ending the field shows the list as the document spells it.
template <typename Parameter>
void bindNumberListEdit(CompositionSession& session, kit::KLineEdit& editor, Parameter parameter) {
    auto& policy = editor.useInlineEdit([](const QString& text) {
        QString reason;
        (void)parseNumberList(text, &reason);
        return reason;
    });
    const auto held = [&session, parameter]() -> std::optional<document::NumberListValue> {
        const auto* composition = session.composition();
        const auto* record =
            composition == nullptr ? nullptr : composition->parameters().find(parameter());
        const auto* constant =
            record == nullptr ? nullptr
                              : std::get_if<document::ConstantValueSource>(&record->source);
        const auto* list = constant == nullptr
                               ? nullptr
                               : std::get_if<document::NumberListValue>(&constant->value);
        return list == nullptr ? std::nullopt : std::optional(*list);
    };
    const auto showHeld = [&editor, held] {
        if (const auto list = held())
            editor.setText(numberListText(*list));
    };
    QObject::connect(&policy, &kit::KInlineEdit::accepted, &editor,
                     [&session, parameter, held, showHeld](const QString& committed) {
                         const auto id = parameter();
                         const auto list = parseNumberList(committed);
                         if (id != document::ParameterId{} && list && held() != list)
                             (void)session.setParameterValue(
                                 id, document::ParameterValue{*list},
                                 QCoreApplication::translate("number_list", "Set Number List"));
                         showHeld();
                     });
    QObject::connect(&policy, &kit::KInlineEdit::refused, &editor, showHeld);
}

} // namespace bloom::ui::properties
