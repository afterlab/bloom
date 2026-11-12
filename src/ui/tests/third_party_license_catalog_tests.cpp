#include <bloom/ui/licenses_window.hpp>
#include <bloom/ui/third_party_license_catalog.hpp>

#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QString>
#include <QWidget>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

using namespace bloom::ui;

// A component name in lower case with its punctuation removed: the lock and a license folder can
// spell one component with a hyphen and an underscore.
[[nodiscard]] std::string nameKey(const std::string_view name) {
    std::string key;
    for (const char character : name) {
        if ((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9')) {
            key.push_back(character);
        } else if (character >= 'A' && character <= 'Z') {
            key.push_back(static_cast<char>(character - 'A' + 'a'));
        }
    }
    return key;
}

[[nodiscard]] std::string withoutCarriageReturns(const std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        if (character != '\r') {
            result.push_back(character);
        }
    }
    return result;
}

// Issue #674. What the dependency lock requires the window to show: for every locked component,
// every license and notice file the lock names for it, whatever the file is called. Returns one
// line per thing that is not shown, so an empty result is the pass.
[[nodiscard]] std::vector<std::string>
lockedLicensesNotShown(const std::filesystem::path& repositoryRoot,
                       const std::span<const ThirdPartyLicenseEntry> catalog) {
    std::vector<std::string> missing;
    QFile lockFile(QString::fromStdString(
        (repositoryRoot / "dependencies" / "dependencies.lock.json").generic_string()));
    if (!lockFile.open(QIODevice::ReadOnly)) {
        return {"the dependency lock could not be read"};
    }
    const auto components =
        QJsonDocument::fromJson(lockFile.readAll()).object().value("components").toArray();
    if (components.isEmpty()) {
        return {"the dependency lock lists no component"};
    }
    for (const auto& value : components) {
        const auto component = value.toObject();
        const auto name = component.value("name").toString().toStdString();
        const auto license = component.value("license").toObject();
        const auto licenseFiles = license.value("licenseFiles").toArray();
        if (licenseFiles.isEmpty()) {
            missing.push_back(name + ": the lock names no license file");
            continue;
        }
        // Qt's entry is named for the product; every other entry carries the lock's name.
        const auto entryKey = name == "qt" ? std::string{"qt6"} : nameKey(name);
        const ThirdPartyLicenseEntry* entry = nullptr;
        for (const auto& candidate : catalog) {
            if (nameKey(candidate.component) == entryKey) {
                entry = &candidate;
            }
        }
        if (entry == nullptr) {
            missing.push_back(name + ": no entry in the window");
            continue;
        }
        const auto shown = withoutCarriageReturns(entry->licenseText);
        for (const auto* const list : {"licenseFiles", "noticeFiles"}) {
            for (const auto& file : license.value(list).toArray()) {
                const auto path = file.toObject().value("path").toString();
                QFile text(
                    QString::fromStdString((repositoryRoot / path.toStdString()).generic_string()));
                if (!text.open(QIODevice::ReadOnly)) {
                    missing.push_back(name + ": " + path.toStdString() + " could not be read");
                    continue;
                }
                const auto bytes = text.readAll();
                const auto expected = withoutCarriageReturns(
                    std::string_view{bytes.constData(), static_cast<std::size_t>(bytes.size())});
                if (expected.empty() || shown.find(expected) == std::string::npos) {
                    missing.push_back(name + ": " + path.toStdString() + " is not shown");
                }
            }
        }
    }
    return missing;
}

void testEveryLockedComponentShowsItsLicense(Expectations& expectations,
                                             const std::filesystem::path& repositoryRoot) {
    const auto catalog = thirdPartyLicenseCatalog();
    for (const auto& line : lockedLicensesNotShown(repositoryRoot, catalog)) {
        expectations.expect(false, "a locked component's license is not shown: " + line);
    }

    // The check itself: take a component away, or cut its text short, and it is named. numpy is
    // the one with the most files, none of them called LICENSE.
    const auto reports = [&](const std::vector<ThirdPartyLicenseEntry>& changed,
                             const std::string_view fragment) {
        for (const auto& line : lockedLicensesNotShown(repositoryRoot, changed)) {
            if (line.find(fragment) != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    std::vector<ThirdPartyLicenseEntry> without;
    std::vector<ThirdPartyLicenseEntry> truncated;
    bool found = false;
    for (const auto& entry : catalog) {
        if (std::string_view{entry.component} != "numpy") {
            without.push_back(entry);
            truncated.push_back(entry);
            continue;
        }
        found = true;
        auto shortened = entry;
        shortened.licenseText = shortened.licenseText.substr(0, shortened.licenseText.size() / 2);
        truncated.push_back(shortened);
    }
    expectations.expect(found,
                        "the catalog lists numpy, whose license files are not named LICENSE");
    expectations.expect(reports(without, "numpy: no entry in the window"),
                        "a locked component with no entry is reported");
    expectations.expect(reports(truncated, "numpy: ") && reports(truncated, " is not shown"),
                        "a locked component with one of its named files missing is reported");
}

// The set of component directory names that carry a LICENSE file under one root -- the rule
// generate_third_party_license_catalog.cmake applies to what the dependency lock does not list.
[[nodiscard]] std::set<std::string>
licensedComponentDirectories(const std::filesystem::path& root) {
    std::set<std::string> names;
    if (!std::filesystem::exists(root)) {
        return names;
    }
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.is_directory() && std::filesystem::exists(entry.path() / "LICENSE")) {
            names.insert(entry.path().filename().string());
        }
    }
    return names;
}

// Decision 3 (task U2, issue #118): the catalog lists AT LEAST every component directory present
// in the two license roots, and shows non-empty text for each. "At least" rather than "exactly"
// because the catalog also carries the synthesized Qt/ADR-0014 notice, which has no LICENSE file
// of its own to enumerate here.
void testCatalogCoversEveryLicensedComponentDirectory(Expectations& expectations,
                                                      const std::filesystem::path& repositoryRoot) {
    const auto dependencyComponents =
        licensedComponentDirectories(repositoryRoot / "dependencies" / "licenses");
    const auto kitComponents =
        licensedComponentDirectories(repositoryRoot / "src" / "ui" / "kit" / "third_party");
    expectations.expect(!dependencyComponents.empty(),
                        "the dependency license root really has components to require (fixture "
                        "sanity: a --root miss would silently pass an empty requirement)");
    expectations.expect(!kitComponents.empty(),
                        "the kit third-party license root really has components to require");

    std::set<std::string> catalogComponents;
    std::set<std::string> catalogKeys;
    for (const auto& entry : thirdPartyLicenseCatalog()) {
        catalogComponents.emplace(entry.component);
        catalogKeys.insert(nameKey(entry.component));
        expectations.expect(!entry.licenseText.empty(), std::string{"the catalog entry for "} +
                                                            std::string{entry.component} +
                                                            " has non-empty license text");
    }

    // A locked component is listed under the lock's name, which can differ from its folder's by
    // punctuation.
    for (const auto& name : dependencyComponents) {
        expectations.expect(catalogKeys.contains(nameKey(name)),
                            "the catalog lists dependency-license component: " + name);
    }
    for (const auto& name : kitComponents) {
        expectations.expect(catalogComponents.contains(name),
                            "the catalog lists kit third-party component: " + name);
    }
    // The vendored media sources keep their license next to the source.
    const auto mediaComponents =
        licensedComponentDirectories(repositoryRoot / "src" / "media" / "third_party");
    expectations.expect(!mediaComponents.empty(),
                        "the media third-party root really has components to require");
    for (const auto& name : mediaComponents) {
        expectations.expect(catalogComponents.contains(name),
                            "the catalog lists media third-party component: " + name);
    }
    expectations.expect(catalogComponents.size() == thirdPartyLicenseCatalog().size(),
                        "no component is listed twice");
    expectations.expect(catalogComponents.contains("Qt 6"),
                        "the catalog also carries the Qt/ADR-0014 notice");
#if defined(__linux__)
    // A Linux package carries a few distribution libraries beside Qt; each one's copyright file
    // is in the catalog under its package's name.
    const auto runtimeLicenses = repositoryRoot / "dependencies" / "linux-runtime" / "licenses";
    std::size_t runtimePackages = 0;
    for (const auto& entry : std::filesystem::directory_iterator(runtimeLicenses)) {
        if (!entry.is_directory()) {
            continue;
        }
        ++runtimePackages;
        expectations.expect(catalogComponents.contains(entry.path().filename().string()),
                            "the catalog lists the carried distribution package: " +
                                entry.path().filename().string());
    }
    expectations.expect(runtimePackages != 0U,
                        "the Linux runtime license root really has packages to require");
#endif
    // The Qt entry is the locked Qt's own notice with its license texts: the exact source
    // archive, the LGPL and the GPL it supplements, and the third-party notices.
    for (const auto& entry : thirdPartyLicenseCatalog()) {
        if (entry.component != "Qt 6") {
            continue;
        }
        const auto has = [&entry](const std::string_view text) {
            return entry.licenseText.find(text) != std::string_view::npos;
        };
        expectations.expect(has("qt-everywhere-src-") && has("corresponding source"),
                            "the Qt entry names the exact Qt source archive");
        expectations.expect(has("GNU LESSER GENERAL PUBLIC LICENSE") &&
                                has("GNU GENERAL PUBLIC LICENSE"),
                            "the Qt entry carries the LGPL and the GPL text it supplements");
        expectations.expect(has("Third-party components inside those libraries and plugins") &&
                                has("==== texts/MIT.txt ===="),
                            "the Qt entry carries Qt's third-party notices and their licenses");
    }
}

// The owner's credit for the bundled UI sounds travels with every build: the catalog entry carries
// the exact credit line and the snd.dev terms summary.
void testSoundCreditIsInTheCatalog(Expectations& expectations) {
    const ThirdPartyLicenseEntry* sound = nullptr;
    for (const auto& entry : thirdPartyLicenseCatalog()) {
        if (std::string_view{entry.component} == "snd01-sine")
            sound = &entry;
    }
    expectations.expect(sound != nullptr, "the catalog lists the SND01 sine UI sounds");
    if (sound == nullptr)
        return;
    const std::string_view text{sound->licenseText};
    expectations.expect(
        text.find("UI sounds: SND01 \"sine\" by Yasuhiro Tsuchiya, from SND (snd.dev) by Dentsu "
                  "Inc. CX Creative Center.") != std::string_view::npos,
        "the sound credit line is present verbatim");
    expectations.expect(text.find("Licence: snd.dev Terms of Use") != std::string_view::npos &&
                            text.find("source code under the MIT licence") !=
                                std::string_view::npos &&
                            text.find("Licence: MIT") == std::string_view::npos,
                        "the sounds are under the snd.dev Terms; only snd-lib's source is MIT");
    expectations.expect(text.find("unprocessed materials") != std::string_view::npos &&
                            text.find("trademark sound logo") != std::string_view::npos,
                        "the snd.dev terms summary is present");
}

void testLicensesWindowListsEveryCatalogEntryWithText(Expectations& expectations) {
    QWidget owner;
    LicensesWindow window(owner);
    auto* list = window.findChild<QListWidget*>(QStringLiteral("licensesComponentList"));
    auto* text = window.findChild<QPlainTextEdit*>(QStringLiteral("licensesTextView"));
    expectations.expect(list != nullptr && text != nullptr,
                        "the window exposes its list and text panes");
    if (list == nullptr || text == nullptr) {
        return;
    }

    expectations.expect(static_cast<std::size_t>(list->count()) ==
                            thirdPartyLicenseCatalog().size(),
                        "the list has exactly one row per catalog entry");
    expectations.expect(!text->toPlainText().isEmpty(),
                        "selecting the first component (the default selection) shows its text");
    expectations.expect(text->isReadOnly(), "the license text is read-only");

    for (int row = 0; row < list->count(); ++row) {
        list->setCurrentRow(row);
        expectations.expect(!text->toPlainText().isEmpty(),
                            "every listed component shows non-empty license text when selected");
    }
}

} // namespace

int main(int argumentCount, char** arguments) {
    std::filesystem::path repositoryRoot = std::filesystem::current_path();
    for (int index = 1; index < argumentCount; ++index) {
        const std::string_view argument{arguments[index]};
        if (argument == "--root" && index + 1 < argumentCount) {
            repositoryRoot = arguments[++index];
        }
    }

    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argumentCount, arguments);
    Expectations expectations;
    testCatalogCoversEveryLicensedComponentDirectory(expectations, repositoryRoot);
    testEveryLockedComponentShowsItsLicense(expectations, repositoryRoot);
    testSoundCreditIsInTheCatalog(expectations);
    testLicensesWindowListsEveryCatalogEntryWithText(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
