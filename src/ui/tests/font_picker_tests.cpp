// #489: the font picker, the font field and the Qt network transport (against a loopback server;
// TLS and the live Google Fonts service are not exercised here).
#include "font_picker.hpp"
#include "qt_font_transport.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <bloom/platform/font_catalog.hpp>
#include <bloom/runtime/font_resolver.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/text_field.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/properties_editor.hpp>

#include <iostream>

using namespace bloom;

namespace {
int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

// Serves one canned response per connection on the calling thread's event loop (the transport spins
// the same loop while it waits).
struct Loopback {
    QTcpServer server;
    QByteArray response;
    bool stall = false;
    Loopback() {
        (void)server.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            auto* socket = server.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                socket->readAll();
                if (stall)
                    return;
                socket->write(response);
                socket->disconnectFromHost();
            });
        });
    }
    [[nodiscard]] std::string url() const {
        return "http://127.0.0.1:" + std::to_string(server.serverPort()) + "/font.ttf";
    }
};

void testTransport() {
    using runtime::fonts::FontHttpStatus;
    ui::QtFontTransport transport;
    const auto never = [] { return false; };
    {
        Loopback loop;
        loop.response = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello";
        std::uint64_t seen = 0;
        const auto result = transport.get(loop.url(), 1024, never,
                                          [&](std::uint64_t received, std::uint64_t) { seen = received; });
        expect(result.status == FontHttpStatus::Ok && result.body.size() == 5, "a 200 delivers the body");
        expect(seen == 5, "progress reports the bytes received");
    }
    {
        Loopback loop;
        loop.response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        const auto result = transport.get(loop.url(), 1024, never, {});
        expect(result.status == FontHttpStatus::HttpError && result.httpStatus == 404,
               "a 404 is an HTTP error, not offline");
    }
    {
        Loopback loop;
        loop.response = "HTTP/1.1 200 OK\r\nContent-Length: 64\r\nConnection: close\r\n\r\n" +
                        QByteArray(64, 'x');
        const auto result = transport.get(loop.url(), 16, never, {});
        expect(result.status == FontHttpStatus::TooLarge, "a body over the cap is refused");
    }
    {
        Loopback loop;
        loop.stall = true;
        QElapsedTimer clock;
        clock.start();
        const auto result =
            transport.get(loop.url(), 1024, [&] { return clock.elapsed() > 150; }, {});
        expect(result.status == FontHttpStatus::Cancelled, "cancellation aborts a stalled transfer");
    }
    {
        std::string url;
        {
            Loopback loop;
            url = loop.url();
        } // The port is closed again.
        const auto result = transport.get(url, 1024, never, {});
        expect(result.status == FontHttpStatus::Offline, "a refused connection reads as offline");
    }
}

void testFieldAndPicker() {
    platform::FontCatalogue installed;
    installed.status = platform::FontCatalogueStatus::Available;
    platform::FontFace face;
    face.family = "Test Sans";
    face.style = "Regular";
    face.weight = 400;
    face.postscriptName = "TestSans-Regular";
    installed.faces.push_back(face);
    runtime::fonts::processFontResolver().systemFonts().install(installed);

    ui::FontField field;
    field.show();
    document::FontId google;
    google.source = document::FontSource::Google;
    google.family = "Roboto";
    google.style = "Bold";
    google.weight = 700;
    field.setFontText(QString::fromStdString(document::encodeFontId(google)));
    expect(field.familyDropdown()->currentText() == "Roboto", "the field shows the family");
    expect(field.styleDropdown()->count() > 1, "a Google family offers several styles");
    expect(field.styleDropdown()->currentText() == "Bold", "the style selector shows the stored style");

    QSignalSpy previewed(&field, &ui::FontField::fontPreviewed);
    QSignalSpy cancelled(&field, &ui::FontField::previewCancelled);
    QSignalSpy committed(&field, &ui::FontField::fontCommitted);

    ui::FontPickerPopup popup;
    QSignalSpy popupPreview(&popup, &ui::FontPickerPopup::previewed);
    QSignalSpy popupCancel(&popup, &ui::FontPickerPopup::cancelled);
    QSignalSpy popupChosen(&popup, &ui::FontPickerPopup::chosen);
    popup.openForField(*field.familyDropdown(), google);
    expect(popup.rowCount() > 100, "the picker lists the baked catalogue and the installed fonts");
    bool sawInstalled = false;
    bool sawGoogle = false;
    for (int row = 0; row < popup.rowCount(); ++row) {
        if (popup.rowFamily(row) == "Test Sans")
            sawInstalled = true;
        if (popup.rowFamily(row) == "Roboto" && popup.rowIsGoogle(row))
            sawGoogle = true;
    }
    expect(sawInstalled && sawGoogle, "installed and Google rows are both listed");
    for (int row = 0; row < popup.rowCount(); ++row)
        if (popup.rowFamily(row) == "Test Sans") {
            popup.list()->setCurrentRow(row);
            break;
        }
    expect(popupPreview.count() >= 1, "moving the current row previews the font");
    popup.close();
    expect(popupCancel.count() == 1, "closing without a choice cancels the preview");

    popup.openForField(*field.familyDropdown(), google);
    for (int row = 0; row < popup.rowCount(); ++row)
        if (popup.rowFamily(row) == "Test Sans") {
            popup.list()->setCurrentRow(row);
            Q_EMIT popup.list()->rowChosen(row);
            break;
        }
    expect(popupChosen.count() == 1, "choosing emits one choice");
    expect(popupCancel.count() == 1, "a choice is not a cancel");
    (void)previewed;
    (void)cancelled;
    (void)committed;
}
// A commit after a live preview keeps the font that was chosen. Dropping the preview refreshes the
// field to the old font, and the commit used to read the field's own text after that.
void testCommitAfterPreview() {
    auto project = document::makeNewProject("Fonts", "Main", core::RationalTime::fromInteger(10));
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, project.initialCompositionId);
    (void)session.addTextLayer("Title", "Hello", 72, {1, 1, 1, 1});
    ui::PropertiesEditor properties(session);
    properties.resize(900, 2200);
    properties.show();
    QCoreApplication::processEvents();
    auto* field = properties.findChild<ui::FontField*>();
    expect(field != nullptr, "the text layer's Properties have a font field");
    if (field == nullptr)
        return;
    const auto before = field->fontText();
    Q_EMIT field->familyDropdown()->openRequested();
    QCoreApplication::processEvents();
    auto* popup = field->findChild<ui::FontPickerPopup*>();
    expect(popup != nullptr, "the font field opens its picker");
    if (popup == nullptr)
        return;
    document::FontId chosen;
    chosen.source = document::FontSource::Google;
    chosen.family = "Roboto";
    chosen.style = "Bold";
    chosen.weight = 700;
    Q_EMIT popup->previewed(chosen);
    QCoreApplication::processEvents();
    expect(session.valueEditActive(),
           "a preview is a live edit, not a commit");
    const auto revision = session.snapshot().revision();
    Q_EMIT popup->chosen(chosen);
    QCoreApplication::processEvents();
    expect(field->fontId().family == "Roboto",
           "a commit after a preview keeps the chosen font, got " + field->fontId().family);
    expect(session.snapshot().revision() != revision, "the commit is an edit");
    expect(session.undo(), "the commit can be undone");
    QCoreApplication::processEvents();
    expect(field->fontText() == before, "one undo puts the earlier font back");
    popup->close();
}

// Review tool, not a test: font_picker_test --capture <dir> <dpr>. Writes the Properties font row,
// the text node's card and the open picker at 2000, 900 and 560 pixels wide. It reaches Google Fonts
// like the application does (a real network), into the per-user cache.
void capture(const QString& directory, const QString& dprText) {
    // The desktop application installs this transport at start-up; with it the picker's Google rows
    // fetch their name-only subsets (set BLOOM_FONT_CAPTURE_OFFLINE to leave them out).
    if (qEnvironmentVariableIsEmpty("BLOOM_FONT_CAPTURE_OFFLINE"))
        runtime::fonts::processFontResolver().setTransport(std::make_shared<ui::QtFontTransport>());
    auto project = document::makeNewProject("Fonts", "Main", core::RationalTime::fromInteger(10));
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, project.initialCompositionId);
    (void)session.addTextLayer("Title", "Hello", 72, {1, 1, 1, 1});
    document::FontId chosen;
    chosen.source = document::FontSource::Google;
    chosen.family = "Roboto";
    chosen.style = "Bold";
    chosen.weight = 700;
    const auto layer = std::get<document::LayerId>(session.selection().primary);
    const auto source = session.directSourceNodeForLayer(layer);
    const QString suffix = QString(dprText).remove(QLatin1Char('.'));
    for (const int width : {2000, 900, 560}) {
        ui::PropertiesEditor properties(session);
        properties.resize(width, 2200);
        properties.show();
        QCoreApplication::processEvents();
        properties.grab().save(QStringLiteral("%1/properties-w%2-dpr%3.png").arg(directory).arg(width).arg(suffix));
        if (auto* field = properties.findChild<ui::FontField*>()) {
            (void)chosen;
            auto* popup = new ui::FontPickerPopup(&properties);
            popup->openForField(*field->familyDropdown(),
                                document::decodeFontId(field->fontText().toStdString())
                                    .value_or(document::defaultFontId()));
            for (int i = 0; i < 12; ++i) {
                QCoreApplication::processEvents();
                QThread::msleep(50);
            }
            popup->grab().save(QStringLiteral("%1/picker-w%2-dpr%3.png").arg(directory).arg(width).arg(suffix));
            // The Google Fonts section: each row in its own typeface once its name-only subset
            // has arrived.
            for (int row = 0; row < popup->rowCount(); ++row)
                if (popup->rowIsGoogle(row)) {
                    popup->list()->setCurrentRow(row + 5);
                    break;
                }
            for (int i = 0; i < 80; ++i) {
                QCoreApplication::processEvents();
                QThread::msleep(50);
            }
            popup->grab().save(QStringLiteral("%1/picker-google-w%2-dpr%3.png").arg(directory).arg(width).arg(suffix));
            // A search: families for many scripts, whose subsets may hold none of the letters of
            // their own Latin name.
            popup->list()->filterField()->setText(QStringLiteral("Noto Sans"));
            for (int i = 0; i < 80; ++i) {
                QCoreApplication::processEvents();
                QThread::msleep(50);
            }
            popup->grab().save(QStringLiteral("%1/picker-search-w%2-dpr%3.png").arg(directory).arg(width).arg(suffix));
            popup->close();
        }
        ui::NodeGraphEditor editor(session);
        editor.resize(width, 560);
        editor.show();
        if (source)
            session.selectNode(*source);
        QCoreApplication::processEvents();
        if (source)
            if (auto* item = editor.graphScene()->findNodeItem(*source))
                editor.graphView()->centerOn(item);
        editor.graphView()->zoomToActualSize();
        QCoreApplication::processEvents();
        editor.grab().save(QStringLiteral("%1/node-card-w%2-dpr%3.png").arg(directory).arg(width).arg(suffix));
    }
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    if (argc >= 4 && QString::fromLatin1(argv[1]) == QStringLiteral("--capture"))
        qputenv("QT_SCALE_FACTOR", argv[3]);
    QApplication app(argc, argv);
    ui::kit::installKinetikTheme(app);
    if (argc >= 4 && QString::fromLatin1(argv[1]) == QStringLiteral("--capture")) {
        capture(QString::fromLocal8Bit(argv[2]), QString::fromLatin1(argv[3]));
        return 0;
    }
    testTransport();
    testFieldAndPicker();
    testCommitAfterPreview();
    return failures == 0 ? 0 : 1;
}
