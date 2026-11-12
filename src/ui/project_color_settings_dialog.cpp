#include "project_color_settings_dialog.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/host/project_color_choice.hpp>
#include <bloom/host/project_session.hpp>
#include <bloom/ui/kit/dialog.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/dialog_prompts.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/file_chooser.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/text_field.hpp>
#include <bloom/ui/project_host.hpp>

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>
#include <QVBoxLayout>
#include <QWidget>

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

namespace bloom::ui {
namespace {

// A digest for people: its first 12 hex digits; the whole digest is in the tooltip.
[[nodiscard]] QString shortDigest(const core::Sha256Digest& digest) {
    const auto hex = digest.toLowercaseHex();
    return QString::fromUtf8(hex.data(), 12);
}

[[nodiscard]] QString fullDigest(const core::Sha256Digest& digest) {
    const auto hex = digest.toLowercaseHex();
    return QString::fromUtf8(hex.data(), static_cast<qsizetype>(hex.size()));
}

// The settings `selected` (with `workingId`) describes, or nullopt with the reason it cannot.
struct Resolved final {
    std::optional<document::ColorSettings> settings;
    QString reason;
};

[[nodiscard]] Resolved resolve(const document::ColorSettings& original, const QString& currentKey,
                               const QString& selected, const std::string& workingId) {
    // Shared with Python's Host.set_color_settings and MCP's project.set-color-settings.
    const auto config = selected == currentKey ? host::ProjectColorConfig::Current
                        : selected == QStringLiteral("aces")
                            ? host::ProjectColorConfig::AcesCg
                            : host::ProjectColorConfig::BloomNeutral;
    auto resolved = host::resolveProjectColorSettings(original, config, workingId);
    if (resolved.settings)
        return {std::move(resolved.settings), {}};
    // The host's reason is English for every client; the dialog says it in the interface language.
    if (resolved.reason.find("ACES configuration is unavailable") != std::string::npos)
        return {std::nullopt, QObject::tr("The built-in ACES configuration is unavailable in this "
                                          "build, so it cannot be selected.")};
    return {std::nullopt, QObject::tr("The working color space \u201c%1\u201d is not offered by "
                                      "that configuration.")
                              .arg(QString::fromStdString(workingId))};
}

// The configuration file the artist chose and what reading it produced. Reading parses the file in
// the supervised color helper, which can take seconds, so it runs on a worker; the dialog only
// offers OK for a file that was read, unchanged since.
struct FileChoice final {
    bool busy = false;
    std::uint64_t generation = 0;
    QString path;
    bool projectRelative = false;
    std::optional<document::ColorSettings> settings;
    QString reason;
};

[[nodiscard]] std::string utf8(const QString& text) { return text.toUtf8().toStdString(); }

[[nodiscard]] QString plainText(const std::string& text) { return QString::fromUtf8(text.c_str()); }

} // namespace

void showProjectColorSettingsDialog(ProjectHost& host, QWidget& owner) {
    const auto* current = host.colorSettings();
    if (current == nullptr)
        return;
    const auto original = *current;

    auto* dialog = new kit::KDialog(QObject::tr("Project Settings — Color"),
                                    QStringLiteral("projectColorSettingsDialog"), owner);
    auto* form = dialog->addForm();
    auto* config = new kit::KDropdown(dialog);
    config->setObjectName(QStringLiteral("projectColorConfigPicker"));
    config->setAccessibleName(QObject::tr("OCIO configuration"));
    config->addItem(QObject::tr("Bloom Neutral v1"), QStringLiteral("neutral"));
    config->addItem(QObject::tr("ACES 1.3 CG (OCIO built-in)"), QStringLiteral("aces"));
    const auto* const currentBuiltin =
        std::get_if<document::BuiltInOcioConfigLocator>(&original.ocioConfig.locator);
    const auto currentUri = currentBuiltin == nullptr ? std::string_view{} : currentBuiltin->uri;
    const auto currentKey = currentUri == color::kAcesCgV1ConfigUri ? QStringLiteral("aces")
                            : currentUri == color::kBloomNeutralV1ConfigUri
                                ? QStringLiteral("neutral")
                                : QStringLiteral("current");
    if (currentKey == QStringLiteral("current"))
        config->addItem(QObject::tr("External configuration (kept as it is)"), currentKey);
    config->addItem(QObject::tr("Configuration file…"), QStringLiteral("file"));
    config->setCurrentIndex(config->findData(currentKey));
    (void)form->addRow(QObject::tr("OCIO Config"), config);

    // The file row: a path, Browse, and Read, which parses the file off the UI thread.
    auto* filePath = new kit::KTextField(dialog);
    filePath->setObjectName(QStringLiteral("projectColorConfigFile"));
    filePath->setAccessibleName(QObject::tr("OCIO configuration file"));
    filePath->setPlaceholderText(QObject::tr("config.ocio or an .ocioz archive"));
    auto* browse = new kit::KButton(QObject::tr("Browse…"), dialog);
    browse->setObjectName(QStringLiteral("projectColorConfigBrowse"));
    auto* read = new kit::KButton(QObject::tr("Read"), dialog);
    read->setObjectName(QStringLiteral("projectColorConfigRead"));
    auto* fileRow = new QWidget(dialog);
    auto* fileLayout = new QHBoxLayout(fileRow);
    fileLayout->setContentsMargins(0, 0, 0, 0);
    fileLayout->addWidget(filePath, 1);
    fileLayout->addWidget(browse);
    fileLayout->addWidget(read);
    (void)form->addRow(QObject::tr("File"), fileRow);
    const auto projectPath = host.displayPath();
    const auto projectDirectory =
        projectPath ? projectPath->parent_path() : std::filesystem::path{};
    auto* inProject = new kit::KCheckBox(
        QObject::tr("The file is inside the project folder (stored relative to it)"), dialog);
    inProject->setObjectName(QStringLiteral("projectColorConfigRelative"));
    inProject->setEnabled(!projectDirectory.empty());
    (void)form->addRow(inProject);
    // What is in force now, for a project that already uses a file.
    const auto describeCurrent = [original]() -> QString {
        if (std::holds_alternative<document::BuiltInOcioConfigLocator>(
                original.ocioConfig.locator))
            return {};
        const auto description = host::describeProjectColorConfig(original);
        return QObject::tr("In use: %1\n%2")
            .arg(plainText(description.kind), plainText(description.location));
    };
    const auto fileChoice = std::make_shared<FileChoice>();

    auto* working = new kit::KDropdown(dialog);
    working->setObjectName(QStringLiteral("projectWorkingColorSpacePicker"));
    working->setAccessibleName(QObject::tr("Working color space"));
    (void)form->addRow(QObject::tr("Working Color Space"), working);
    auto* summary = new kit::KLabel(dialog);
    summary->setObjectName(QStringLiteral("projectColorSummary"));
    summary->setWordWrap(true);
    summary->setTone(kit::Color::Muted);
    (void)form->addRow(summary);
    auto* hint = new kit::KLabel(
        QObject::tr("Choose a configuration file to use an OCIO configuration from disk: a "
                    "config.ocio or an .ocioz archive. Choosing Bloom Neutral or ACES stops "
                    "using it. The change is one undoable step."),
        dialog, kit::TypeRole::UiSmall);
    hint->setObjectName(QStringLiteral("projectColorHint"));
    hint->setWordWrap(true);
    hint->setTone(kit::Color::Muted);
    dialog->contentLayout()->addWidget(hint);
    dialog->contentLayout()->addStretch(1);

    // The label an artist reads ("Linear Rec.709 (sRGB)"); the item data stays the stored id.
    const auto addSpace = [working](const std::string_view id) {
        const auto label = host::workingColorSpaceLabel(id);
        working->addItem(QString::fromUtf8(label.data(), static_cast<qsizetype>(label.size())),
                         QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size())));
    };
    const auto populate = [config, working, summary, original, currentKey, addSpace, fileChoice,
                           describeCurrent, fileRow, inProject] {
        const auto selected = config->currentData().toString();
        const bool isFile = selected == QStringLiteral("file");
        fileRow->setVisible(isFile);
        inProject->setVisible(isFile);
        working->clearItems();
        core::Sha256Digest digest = original.ocioConfig.expectedRevision.digest;
        QString name;
        QString displays;
        if (isFile) {
            const auto& chosen = fileChoice->settings;
            const auto shown = chosen ? *chosen : original;
            addSpace(shown.processColorSpaceId);
            if (chosen)
                digest = chosen->ocioConfig.expectedRevision.digest;
            name = fileChoice->busy ? QObject::tr("Reading the configuration…")
                   : chosen        ? QObject::tr("Configuration file")
                   : !fileChoice->reason.isEmpty()
                       ? fileChoice->reason
                       : QObject::tr("Choose a file, then Read it.");
        } else if (selected == QStringLiteral("current")) {
            addSpace(original.processColorSpaceId);
            name = describeCurrent().isEmpty() ? QObject::tr("External configuration")
                                               : describeCurrent();
        } else if (selected == QStringLiteral("aces")) {
            for (const auto id : host::projectWorkingColorSpaces(host::ProjectColorConfig::AcesCg))
                addSpace(id);
            if (selected != currentKey)
                if (const auto revision = color::ocioBuiltInContentRevision(
                        color::OcioConfigLocatorKind::BloomBuiltIn, color::kAcesCgV1ConfigUri))
                    digest = *revision;
            name = QObject::tr("ACES 1.3 CG (cg-config v1.0.0, OCIO 2.1)");
            displays = QObject::tr("sRGB - Display: ACES 1.0 - SDR Video, Un-tone-mapped, Raw");
        } else {
            for (const auto id :
                 host::projectWorkingColorSpaces(host::ProjectColorConfig::BloomNeutral))
                addSpace(id);
            if (selected != currentKey)
                digest = color::kBloomNeutralV1ConfigDigest;
            name = QObject::tr("Bloom Neutral v1");
            displays = QObject::tr("sRGB - Display");
        }
        QStringList lines{name};
        if (!displays.isEmpty())
            lines << QObject::tr("Displays: %1").arg(displays);
        if (!isFile || fileChoice->settings)
            lines << QObject::tr("Revision %1…").arg(shortDigest(digest));
        summary->setText(lines.join(QLatin1Char('\n')));
        summary->setToolTip(QObject::tr("SHA-256 %1").arg(fullDigest(digest)));
        if (isFile) {
            working->setCurrentIndex(0);
        } else if (selected == currentKey) {
            const auto previous = QString::fromStdString(original.processColorSpaceId);
            if (working->findData(previous) < 0)
                addSpace(original.processColorSpaceId);
            working->setCurrentIndex(working->findData(previous));
        } else {
            working->setCurrentIndex(0);
        }
    };
    QObject::connect(config, &kit::KDropdown::currentIndexChanged, dialog,
                     [populate] { populate(); });
    // Reading a file: parsed in the supervised color helper on a worker, never on this thread.
    QPointer<kit::KDialog> alive(dialog);
    const auto startRead = [=] {
        if (fileChoice->busy)
            return;
        fileChoice->path = filePath->text().trimmed();
        fileChoice->projectRelative = inProject->isChecked();
        fileChoice->settings.reset();
        fileChoice->reason.clear();
        if (fileChoice->path.isEmpty()) {
            fileChoice->reason =
                QObject::tr("Choose a color configuration file: a config.ocio or an .ocioz "
                            "archive.");
            populate();
            return;
        }
        host::ProjectColorConfigFile file;
        file.path = utf8(fileChoice->path);
        file.projectRelative = fileChoice->projectRelative;
        if (file.projectRelative) {
            // A file chosen inside the project folder is stored relative to it.
            const auto chosen = std::filesystem::path(file.path);
            if (chosen.is_absolute()) {
                const auto relative = chosen.lexically_relative(projectDirectory);
                if (!relative.empty() && *relative.begin() != "..")
                    file.path = relative.generic_string();
            }
        }
        fileChoice->busy = true;
        const auto generation = ++fileChoice->generation;
        populate();
        QThreadPool::globalInstance()->start([=] {
            auto resolved = host::resolveProjectColorConfigFile(original, file, std::nullopt,
                                                                projectDirectory);
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [=, resolved = std::move(resolved)]() mutable {
                    if (!alive || fileChoice->generation != generation)
                        return;
                    fileChoice->busy = false;
                    fileChoice->settings = std::move(resolved.settings);
                    fileChoice->reason = plainText(resolved.reason);
                    populate();
                },
                Qt::QueuedConnection);
        });
    };
    QObject::connect(read, &QAbstractButton::clicked, dialog, startRead);
    QObject::connect(filePath, &QLineEdit::returnPressed, dialog, startRead);
    QObject::connect(browse, &QAbstractButton::clicked, dialog, [=] {
        const auto start = projectDirectory.empty()
                               ? QString{}
                               : QString::fromStdString(projectDirectory.generic_string());
        if (alive.isNull())
            return;
        const auto chosen = kit::chooseFile(
            *alive.data(), {.mode = kit::KFileRequest::Mode::Open,
                            .title = QObject::tr("Choose an OCIO configuration"),
                            .kind = QStringLiteral("ocio-config"),
                            .nameFilters = QObject::tr("OCIO configuration (config.ocio *.ocioz)"),
                            .defaultSuffix = {},
                            .startPath = start});
        if (!chosen || chosen->isEmpty())
            return;
        filePath->setText(*chosen);
        startRead();
    });
    // Any change to the choice makes the last reading stale.
    const auto staleReading = [=] {
        ++fileChoice->generation;
        fileChoice->busy = false;
        fileChoice->settings.reset();
        fileChoice->reason.clear();
        populate();
    };
    QObject::connect(filePath, &QLineEdit::textEdited, dialog, staleReading);
    QObject::connect(inProject, &QAbstractButton::toggled, dialog, staleReading);
    populate();
    dialog->addButton(kit::KDialog::Role::Accept, QObject::tr("OK"));
    dialog->addButton(kit::KDialog::Role::Reject, QObject::tr("Cancel"));
    dialog->setValidator([config, working, original, currentKey, fileChoice,
                          filePath]() -> std::optional<kit::KRefusal> {
        if (config->currentData().toString() == QStringLiteral("file")) {
            if (fileChoice->settings)
                return std::nullopt;
            return kit::KRefusal{filePath,
                                 fileChoice->busy
                                     ? QObject::tr("Wait for the configuration to be read.")
                                 : !fileChoice->reason.isEmpty()
                                     ? fileChoice->reason
                                     : QObject::tr("Read the configuration file first.")};
        }
        const auto resolved = resolve(original, currentKey, config->currentData().toString(),
                                      working->currentData().toString().toStdString());
        if (!resolved.settings)
            return kit::KRefusal{config, resolved.reason};
        return std::nullopt;
    });
    dialog->watch(config);
    dialog->watch(working);
    dialog->setInitialFocus(config);
    QPointer<ProjectHost> guard(&host);
    QPointer<QWidget> window(&kit::KDialog::ownerWindow(owner));
    QPointer<kit::KDropdown> configField(config);
    QPointer<kit::KDropdown> workingField(working);
    dialog->open([=](const kit::KDialog::Role role) {
        if (role != kit::KDialog::Role::Accept || !guard || !configField || !workingField)
            return;
        const auto selected = configField->currentData().toString();
        const auto workingId = workingField->currentData().toString().toStdString();
        if (selected == currentKey && workingId == original.processColorSpaceId)
            return;
        auto resolved = selected == QStringLiteral("file")
                            ? Resolved{fileChoice->settings, {}}
                            : resolve(original, currentKey, selected, workingId);
        if (!resolved.settings)
            return;
        const auto status = guard->setColorSettings(std::move(*resolved.settings));
        if (status == host::ProjectSessionColorSettingsStatus::InvalidSettings && window)
            kit::inform(*window, kit::KPromptTone::Error, QObject::tr("Project Settings"),
                        QObject::tr("The color configuration was not changed."),
                        QObject::tr("The selected color configuration could not be resolved."),
                        QStringLiteral("projectColorSettingsFailed"));
    });
}

} // namespace bloom::ui
