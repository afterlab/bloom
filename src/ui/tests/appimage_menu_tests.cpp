// The AppImage's "Add to Application Menu" offer (#734): the files it writes into a data folder of
// the test's own, the offer's once-only rule, a moved file's silent refresh and a path the entry
// cannot carry. Capture mode (the environment variable named below) writes the review pictures of
// the prompt instead of asserting. Nothing here reads or writes the real home folder.
#include "await_qt.hpp"

#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/appimage_menu_entry.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/dialog_prompts.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QApplication>
#include <QDir>
#include <QPixmap>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QWidget>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#ifndef BLOOM_APPIMAGE_MENU_SOURCE
#error "BLOOM_APPIMAGE_MENU_SOURCE must name apps/bloom/linux"
#endif

using namespace bloom;
using bloom::test_support::awaitQtCondition;
namespace fs = std::filesystem;

namespace {

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

constexpr auto kAnsweredKey = "appimage/menu-offer-answered";

[[nodiscard]] std::string readAll(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void writeAll(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

[[nodiscard]] bool hasLine(const std::string& text, const std::string& line) {
    return ("\n" + text).find("\n" + line + "\n") != std::string::npos;
}

[[nodiscard]] bool present(const fs::path& path) {
    std::error_code error;
    return fs::exists(path, error);
}

[[nodiscard]] std::vector<fs::path> allFiles(const fs::path& folder) {
    std::vector<fs::path> files;
    std::error_code error;
    for (fs::recursive_directory_iterator it(folder, error), end; !error && it != end;
         it.increment(error)) {
        if (it->is_regular_file()) {
            files.push_back(it->path());
        }
    }
    return files;
}

// One run of the offer: a mounted tree built from the package's own files, an AppImage file, and
// a data folder, all under a folder made for the run.
struct Bench final {
    QTemporaryDir folder;
    fs::path root;
    fs::path mounted;
    fs::path data;
    runtime::TaskScheduler scheduler;
    ui::TaskUiBridge bridge{scheduler};
    QWidget owner;
    int refreshes = 0;

    Bench() : root(folder.path().toStdString()), mounted(root / "mount"), data(root / "data") {
        const fs::path source = BLOOM_APPIMAGE_MENU_SOURCE;
        const std::string application = "id.afterlab.bloom";
        writeAll(mounted / "share/applications" / (application + ".desktop"),
                 readAll(source / (application + ".desktop")));
        writeAll(mounted / "share/mime/packages" / (application + ".xml"),
                 readAll(source / (application + ".xml")));
        for (const auto& entry : fs::directory_iterator(source / "icons")) {
            writeAll(mounted / "share/icons/hicolor" / entry.path().filename() / "apps" /
                         (application + ".png"),
                     readAll(entry.path() / (application + ".png")));
        }
        owner.resize(400, 300);
        owner.show();
    }

    [[nodiscard]] fs::path appImage(const std::string& name = "Bloom.AppImage") const {
        const auto path = root / "My Apps" / name;
        if (!present(path)) {
            writeAll(path, "image");
        }
        return path;
    }
    [[nodiscard]] fs::path entry() const {
        return data / "applications" / "id.afterlab.bloom.desktop";
    }
    [[nodiscard]] ui::AppImageMenuEnvironment environment(const fs::path& image) {
        ui::AppImageMenuEnvironment environment;
        environment.appImage = image;
        environment.mountedTree = mounted;
        environment.dataHome = data;
        environment.refreshIndexes = [this](const fs::path&) { ++refreshes; };
        return environment;
    }
    [[nodiscard]] ui::kit::KDialog*
    prompt(const QString& name = QStringLiteral("appImageMenuOffer")) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
        return owner.findChild<ui::kit::KDialog*>(name);
    }
};

void forgetAnswer() { QSettings().remove(QLatin1String(kAnsweredKey)); }
[[nodiscard]] bool answered() { return QSettings().contains(QLatin1String(kAnsweredKey)); }

void testWrite() {
    Bench bench;
    const auto image = bench.appImage();
    const auto result = ui::writeAppImageMenuEntry(image, bench.mounted, bench.data);
    expect(result.succeeded() && result.refusal.empty(), "a plain AppImage path is written");
    const auto text = readAll(bench.entry());
    expect(hasLine(text, "TryExec=" + image.string()), "the entry names the file in TryExec");
    expect(hasLine(text, "Exec=\"" + image.string() + "\" %f"),
           "the entry starts the file by its quoted full path, with %f");
    expect(hasLine(text, "X-Bloom-AppImage=true"), "the entry carries Bloom's marker");
    expect(!hasLine(text, "Exec=Bloom %f"), "the package's relative Exec line is gone");
    expect(hasLine(text, "Name=Bloom") && hasLine(text, "Icon=id.afterlab.bloom") &&
               hasLine(text, "MimeType=application/x-bloom-project;"),
           "the rest of the package's entry is kept");
    expect(readAll(bench.data / "mime/packages/id.afterlab.bloom.xml") ==
               readAll(bench.mounted / "share/mime/packages/id.afterlab.bloom.xml"),
           "the file type is copied as it is");
    int icons = 0;
    for (const auto& entry : fs::directory_iterator(bench.mounted / "share/icons/hicolor")) {
        const auto copy = bench.data / "icons/hicolor" / entry.path().filename() / "apps" /
                          "id.afterlab.bloom.png";
        expect(present(copy) &&
                   readAll(copy) == readAll(entry.path() / "apps/id.afterlab.bloom.png"),
               "icon " + entry.path().filename().string() + " is copied as it is");
        ++icons;
    }
    expect(icons >= 2, "the fixture has several icon sizes");
    expect(result.written.size() == static_cast<std::size_t>(icons) + 2,
           "the result lists every file it wrote");
    expect(allFiles(bench.data).size() == result.written.size(),
           "nothing else was written, and no half-written file is left");
    expect(ui::appImageEntryState(image, bench.data) == ui::AppImageEntryState::Current,
           "the written entry is current for its file");
    expect(ui::appImageEntryState(bench.appImage("Other.AppImage"), bench.data) ==
               ui::AppImageEntryState::Moved,
           "the written entry is out of date for another file");
    // Written again for a moved file: replaced, not duplicated.
    const auto other = bench.appImage("Renamed.AppImage");
    expect(ui::writeAppImageMenuEntry(other, bench.mounted, bench.data).succeeded(),
           "an entry is written again");
    const auto rewritten = readAll(bench.entry());
    expect(hasLine(rewritten, "Exec=\"" + other.string() + "\" %f") &&
               !hasLine(rewritten, "TryExec=" + image.string()) &&
               std::count(rewritten.begin(), rewritten.end(), '\n') ==
                   std::count(text.begin(), text.end(), '\n'),
           "the rewritten entry names the new file only");
}

void testRefusals() {
    Bench bench;
    for (const char character : {'"', '`', '$', '\\', '%', '\n', '\r'}) {
        const auto image = bench.root / (std::string("a") + character + "b.AppImage");
        const auto result = ui::writeAppImageMenuEntry(image, bench.mounted, bench.data);
        expect(!result.succeeded() && result.written.empty(),
               std::string("a path with character code ") +
                   std::to_string(static_cast<int>(character)) + " is refused");
    }
    expect(!ui::writeAppImageMenuEntry("relative/Bloom.AppImage", bench.mounted, bench.data)
                .succeeded(),
           "a relative path is refused");
    expect(!ui::writeAppImageMenuEntry(bench.appImage(), bench.root / "nothing", bench.data)
                .succeeded(),
           "a mounted tree without the entry is refused");
    expect(!present(bench.data), "a refusal writes nothing, not even the folder");
}

void testOfferOnlyForAnAppImage() {
    Bench bench;
    forgetAnswer();
    ui::AppImageMenuOffer offer(bench.scheduler, bench.bridge, bench.environment({}), bench.owner);
    offer.start();
    expect(bench.prompt() == nullptr && !offer.busy() && !answered(),
           "a program that is not an AppImage is not asked");
    // The environment of the running program, for any other kind of install.
    update::InstallFacts facts;
    facts.kind = update::InstallKind::UserFolder;
    expect(ui::applicationAppImageMenuEnvironment(facts).appImage.empty(),
           "only the AppImage kind has an application environment");
}

void testNotNow() {
    Bench bench;
    forgetAnswer();
    {
        ui::AppImageMenuOffer offer(bench.scheduler, bench.bridge,
                                    bench.environment(bench.appImage()), bench.owner);
        offer.start();
        auto* prompt = bench.prompt();
        expect(prompt != nullptr && prompt->isVisible(), "an AppImage is offered the entry");
        expect(!offer.busy() && !present(bench.data), "nothing is written before the answer");
        if (prompt != nullptr) {
            prompt->button(ui::kit::KDialog::Role::Reject)->click();
        }
        QCoreApplication::processEvents();
        expect(answered(), "Not Now records the answer");
        expect(!offer.busy() && !present(bench.data), "Not Now writes nothing");
    }
    ui::AppImageMenuOffer again(bench.scheduler, bench.bridge, bench.environment(bench.appImage()),
                                bench.owner);
    again.start();
    expect(bench.prompt() == nullptr && !again.busy(), "the offer is made once");
    expect(bench.refreshes == 0, "no index was refreshed");
}

void testAdd() {
    Bench bench;
    forgetAnswer();
    const auto image = bench.appImage();
    int finished = 0;
    bool announced = false;
    {
        ui::AppImageMenuOffer offer(bench.scheduler, bench.bridge, bench.environment(image),
                                    bench.owner);
        QObject::connect(&offer, &ui::AppImageMenuOffer::writeFinished, &offer,
                         [&](const bool succeeded, const bool wasAsked) {
                             finished += succeeded ? 1 : 100;
                             announced = wasAsked;
                         });
        offer.start();
        auto* prompt = bench.prompt();
        expect(prompt != nullptr, "the offer is shown");
        if (prompt != nullptr) {
            prompt->button(ui::kit::KDialog::Role::Accept)->click();
        }
        expect(awaitQtCondition("the write ends", [&] { return finished != 0; }) && finished == 1 &&
                   announced,
               "Add to Application Menu writes the entry");
        expect(answered(), "Add records the answer");
        expect(bench.refreshes == 1, "the desktop's indexes are refreshed once");
    }
    expect(ui::appImageEntryState(image, bench.data) == ui::AppImageEntryState::Current,
           "the entry was written for the running file");
    ui::AppImageMenuOffer again(bench.scheduler, bench.bridge, bench.environment(image),
                                bench.owner);
    again.start();
    expect(bench.prompt() == nullptr && !again.busy() && bench.refreshes == 1,
           "an entry that is current is left alone, and nobody is asked");
}

void testOtherEntriesAreLeftAlone() {
    Bench bench;
    forgetAnswer();
    writeAll(bench.entry(), "[Desktop Entry]\nName=Mine\nExec=\"/opt/other/Bloom\" %f\n");
    ui::AppImageMenuOffer offer(bench.scheduler, bench.bridge, bench.environment(bench.appImage()),
                                bench.owner);
    offer.start();
    expect(bench.prompt() == nullptr && !offer.busy() && !answered(),
           "an entry that is not Bloom's own AppImage entry is neither offered over nor rewritten");
    expect(hasLine(readAll(bench.entry()), "Name=Mine"), "that entry is untouched");
}

void testMovedFile() {
    Bench bench;
    forgetAnswer();
    const auto first = bench.appImage("Bloom-1.AppImage");
    expect(ui::writeAppImageMenuEntry(first, bench.mounted, bench.data).succeeded(),
           "the first entry is written");
    const auto moved = bench.appImage("Moved/Bloom-1.AppImage");
    int finished = 0;
    ui::AppImageMenuOffer offer(bench.scheduler, bench.bridge, bench.environment(moved),
                                bench.owner);
    QObject::connect(&offer, &ui::AppImageMenuOffer::writeFinished, &offer,
                     [&](const bool succeeded, const bool wasAsked) {
                         finished += succeeded && !wasAsked ? 1 : 100;
                     });
    offer.start();
    expect(bench.prompt() == nullptr, "a moved file is refreshed without a question");
    expect(awaitQtCondition("the refresh ends", [&] { return finished != 0; }) && finished == 1,
           "the entry is written again in the background");
    expect(hasLine(readAll(bench.entry()), "Exec=\"" + moved.string() + "\" %f") &&
               hasLine(readAll(bench.entry()), "TryExec=" + moved.string()),
           "the entry names the file where it is now");
    expect(!answered(), "a refresh does not count as an answer");
}

void testRefusedPath() {
    Bench bench;
    forgetAnswer();
    const auto image = bench.root / "My$Apps" / "Bloom.AppImage";
    int finished = 0;
    ui::AppImageMenuOffer offer(bench.scheduler, bench.bridge, bench.environment(image),
                                bench.owner);
    QObject::connect(&offer, &ui::AppImageMenuOffer::writeFinished, &offer,
                     [&](const bool succeeded, const bool) { finished += succeeded ? 100 : 1; });
    offer.start();
    auto* prompt = bench.prompt();
    expect(prompt != nullptr, "the offer is shown for a path the entry cannot carry");
    if (prompt != nullptr) {
        prompt->button(ui::kit::KDialog::Role::Accept)->click();
    }
    expect(awaitQtCondition("the write ends", [&] { return finished != 0; }) && finished == 1,
           "the write is refused");
    expect(bench.prompt(QStringLiteral("appImageMenuFailed")) != nullptr,
           "the artist is told why nothing was added");
    expect(!present(bench.data) && bench.refreshes == 0, "nothing was written or refreshed");
}

// The review picture of the prompt, at the device pixel ratio the run was started with.
void capture(const QString& directory) {
    Bench bench;
    forgetAnswer();
    ui::AppImageMenuOffer offer(bench.scheduler, bench.bridge, bench.environment(bench.appImage()),
                                bench.owner);
    offer.start();
    auto* prompt = bench.prompt();
    if (prompt == nullptr) {
        std::cerr << "FAIL: no prompt to capture\n";
        ++failures;
        return;
    }
    prompt->adjustSize();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    QCoreApplication::processEvents();
    const auto name = QStringLiteral("734-after-appimage-menu-offer-%1.png")
                          .arg(prompt->devicePixelRatioF() > 1.0 ? QStringLiteral("dpr1.5")
                                                                 : QStringLiteral("dpr1"));
    (void)QDir().mkpath(directory);
    if (!prompt->grab().save(QDir(directory).filePath(name))) {
        std::cerr << "FAIL: the capture could not be saved\n";
        ++failures;
    }
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const QTemporaryDir home;
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("BloomAppImageMenuTests"));
    QCoreApplication::setApplicationName(QStringLiteral("appimage-menu-tests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, home.path());
    QApplication application(argc, argv);
    if (const auto directory = qEnvironmentVariable("BLOOM_APPIMAGE_MENU_CAPTURE_DIR");
        !directory.isEmpty()) {
        capture(directory);
        return failures == 0 ? 0 : 1;
    }
    testWrite();
    testRefusals();
    testOfferOnlyForAnAppImage();
    testNotNow();
    testAdd();
    testOtherEntriesAreLeftAlone();
    testMovedFile();
    testRefusedPath();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
