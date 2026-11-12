// #706: a popup's drop shadow is the soft blur the kit paints, not a hard dark edge. A popup, a
// menu and a tooltip are frameless windows that ask for no system shadow of their own (Windows and
// macOS would add one around the translucent frame, as a solid dark border), and the painted
// shadow fades out across its gutter. The test also writes a dropdown with its shadow as a PNG over
// a light and a dark plate, for review on every platform:
//   bloom_popup_shadow_test [output.png]   (or BLOOM_POPUP_SHADOW_PNG; default
//   popup-shadow-<platform>.png in the working directory)
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/popup_window.hpp>
#include <bloom/ui/kit/theme.hpp>

#include <QApplication>
#include <QDir>
#include <QImage>
#include <QMenu>
#include <QPainter>
#include <QTest>
#include <QToolTip>
#include <QWidget>

#include <algorithm>
#include <iostream>
#include <set>

namespace {
using namespace bloom::ui;
int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void checkWindowFlags(const QWidget& widget, const std::string& what) {
    const auto flags = widget.windowFlags();
    expect(flags.testFlag(Qt::NoDropShadowWindowHint),
           what + " asks for no system drop shadow around its own");
    expect(flags.testFlag(Qt::FramelessWindowHint), what + " is a frameless window");
}

// The alpha along a line from just outside the frame to the edge of the window.
std::vector<int> alphaProfile(const QImage& image, const QRect& frame, const bool below) {
    std::vector<int> alpha;
    const int x = frame.center().x();
    if (below) {
        for (int y = frame.bottom() + 1; y < image.height(); ++y)
            alpha.push_back(qAlpha(image.pixel(x, y)));
    } else {
        for (int px = frame.right() + 1; px < image.width(); ++px)
            alpha.push_back(qAlpha(image.pixel(px, frame.center().y())));
    }
    return alpha;
}

void checkSoftShadow(const std::vector<int>& alpha, const std::string& side) {
    expect(alpha.size() >= 4, side + " shadow has a gutter to fade in");
    if (alpha.size() < 4)
        return;
    std::set<int> levels(alpha.begin(), alpha.end());
    expect(levels.size() >= 4, side + " shadow fades through several levels, not one solid edge");
    expect(*std::max_element(alpha.begin(), alpha.end()) < 200, side + " shadow is translucent, never solid black");
    bool falling = true;
    for (std::size_t index = 1; index < alpha.size(); ++index)
        falling = falling && alpha[index] <= alpha[index - 1] + 1;
    expect(falling, side + " shadow only fades away from the frame");
    expect(alpha.back() <= 2, side + " shadow ends in full transparency at the window's edge");
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    kit::installAfterlabTheme(app);
    QApplication::setStyle(new kit::AltUnderlineProxyStyle());
    QWidget window;
    window.resize(360, 320);
    auto* dropdown = new kit::KDropdown(&window);
    for (const char* name : {"Normal", "Add", "Multiply", "Screen", "Overlay", "Darken"})
        dropdown->addItem(QString::fromLatin1(name));
    dropdown->move(40, 30);
    window.show();
    QTest::qWait(60);
    dropdown->showPopup();
    QTest::qWait(250);
    auto* popup = dropdown->findChild<kit::KPopupWindow*>();
    if (popup == nullptr) {
        for (auto* top : QApplication::topLevelWidgets())
            if (auto* candidate = qobject_cast<kit::KPopupWindow*>(top))
                popup = candidate;
    }
    expect(popup != nullptr, "the dropdown opens its popup");
    QImage shot;
    if (popup != nullptr) {
        checkWindowFlags(*popup, "the dropdown popup");
        shot = popup->grab().toImage().convertToFormat(QImage::Format_ARGB32);
        const auto frame = popup->frameRect();
        const auto gutter = kit::popupGutter(kit::Elevation::Popup);
        if (!gutter.isNull()) {
            checkSoftShadow(alphaProfile(shot, frame, true), "the bottom");
            checkSoftShadow(alphaProfile(shot, frame, false), "the right");
        }
    }

    auto* menu = kit::makeMenu(&window);
    menu->addAction(QStringLiteral("Fit"));
    menu->addAction(QStringLiteral("100%"));
    menu->ensurePolished();
    if (kit::popupTranslucencySupported())
        checkWindowFlags(*menu, "a menu");
    QToolTip::showText(window.mapToGlobal(QPoint(10, 10)), QStringLiteral("A tooltip"), &window);
    QTest::qWait(60);
    for (auto* top : QApplication::topLevelWidgets())
        if (top->inherits("QTipLabel") && kit::popupTranslucencySupported())
            checkWindowFlags(*top, "a tooltip");

    // The review capture: the popup over a light plate and over a dark one, then its bottom-right
    // corner over the light plate four times enlarged (pixel for pixel), where the blur is easy to
    // see at a glance.
    if (!shot.isNull()) {
        constexpr int kZoom = 4;
        const QRect corner = popup != nullptr
                                 ? QRect(popup->frameRect().right() - 23,
                                         popup->frameRect().bottom() - 23, 24, 24)
                                       .united(QRect(shot.width() - 1, shot.height() - 1, 1, 1))
                                 : QRect();
        QImage cornerShot(corner.size(), QImage::Format_ARGB32_Premultiplied);
        cornerShot.fill(QColor(236, 236, 236));
        {
            QPainter cornerPainter(&cornerShot);
            cornerPainter.drawImage(QPoint(0, 0), shot, corner);
        }
        const QImage enlarged = cornerShot.scaled(corner.size() * kZoom, Qt::IgnoreAspectRatio,
                                                  Qt::FastTransformation);
        const int plates = shot.width() * 2 + 30;
        QImage sheet(plates + enlarged.width() + 20,
                     std::max(shot.height(), enlarged.height()) + 20,
                     QImage::Format_ARGB32_Premultiplied);
        sheet.fill(Qt::transparent);
        QPainter painter(&sheet);
        painter.fillRect(QRect(0, 0, shot.width() + 15, sheet.height()), QColor(236, 236, 236));
        painter.fillRect(QRect(shot.width() + 15, 0, shot.width() + 15, sheet.height()), QColor(46, 46, 52));
        painter.fillRect(QRect(plates, 0, enlarged.width() + 20, sheet.height()), QColor(236, 236, 236));
        painter.drawImage(QPoint(8, 10), shot);
        painter.drawImage(QPoint(shot.width() + 23, 10), shot);
        painter.drawImage(QPoint(plates + 10, 10), enlarged);
        painter.end();
        if (popup != nullptr) {
            std::cout << "shadow alpha below the frame:";
            for (const int value : alphaProfile(shot, popup->frameRect(), true))
                std::cout << ' ' << value;
            std::cout << '\n';
        }
        QString path = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                : qEnvironmentVariable("BLOOM_POPUP_SHADOW_PNG");
        if (path.isEmpty())
            path = QStringLiteral("popup-shadow-%1.png").arg(QGuiApplication::platformName());
        expect(sheet.save(path), "the shadow capture is written");
        std::cout << "popup shadow capture: " << QDir::current().absoluteFilePath(path).toStdString() << '\n';
    }
    return failures == 0 ? 0 : 1;
}
