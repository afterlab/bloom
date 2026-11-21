// #763: a UI test variant registered for a display scale runs at that scale.
//
// Registered exactly the way every UI scale variant is (src/ui/tests/CMakeLists.txt): offscreen,
// QT_SCALE_FACTOR=<scale>, BLOOM_REDUCED_MOTION=1, each as its own ENVIRONMENT argument. The scale
// the registration names is also passed on the command line, and the test reads what the process
// actually got: the platform Qt chose, the screen's and a shown window's device pixel ratio, the
// pixel size of a grab, and the kit's reduced-motion switch. Before #763 the three variables
// reached the process as one, QT_QPA_PLATFORM="offscreen;QT_SCALE_FACTOR=...;...", which Qt reads as
// a list of platforms to try: the window came up offscreen at ratio 1 with motion on, so every
// check here but the first fails for the 1.5 and 2 variants.
#include <bloom/ui/kit/tokens.hpp>

#include <QApplication>
#include <QGuiApplication>
#include <QPixmap>
#include <QScreen>
#include <QWidget>

#include <cmath>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << what << '\n';
    }
}

bool same(double a, double b) { return std::abs(a - b) < 1e-9; }

} // namespace

int main(int argc, char** argv) {
    double wanted = 0.0;
    for (int index = 1; index + 1 < argc; ++index)
        if (std::string(argv[index]) == "--scale")
            wanted = std::stod(argv[index + 1]);
    if (wanted <= 0.0) {
        std::cerr << "usage: bloom_ui_test_display_scale_test --scale <scale>\n";
        return 2;
    }

    QApplication app(argc, argv);
    const QByteArray platformVariable = qgetenv("QT_QPA_PLATFORM");
    const QByteArray scaleVariable = qgetenv("QT_SCALE_FACTOR");
    std::cout << "QT_QPA_PLATFORM='" << platformVariable.toStdString() << "' QT_SCALE_FACTOR='"
              << scaleVariable.toStdString() << "' BLOOM_REDUCED_MOTION='"
              << qgetenv("BLOOM_REDUCED_MOTION").toStdString() << "'\n";
    expect(platformVariable == "offscreen",
           "QT_QPA_PLATFORM is exactly 'offscreen' (no other variable folded into it)");
    expect(QGuiApplication::platformName() == QStringLiteral("offscreen"),
           "Qt runs the offscreen platform");
    expect(!scaleVariable.isEmpty() && same(scaleVariable.toDouble(), wanted),
           "QT_SCALE_FACTOR is set on its own to the registered scale");
    expect(qgetenv("BLOOM_REDUCED_MOTION") == "1" && !bloom::ui::kit::motionEnabled(),
           "BLOOM_REDUCED_MOTION is set on its own and the kit's motion is off");

    const QScreen* screen = QGuiApplication::primaryScreen();
    expect(screen != nullptr, "there is a screen");
    const double screenRatio = screen != nullptr ? screen->devicePixelRatio() : 0.0;
    QWidget window;
    window.resize(200, 120);
    window.show();
    QCoreApplication::processEvents();
    const double windowRatio = window.devicePixelRatioF();
    const QPixmap grab = window.grab();
    std::cout << "wanted " << wanted << ", screen devicePixelRatio " << screenRatio
              << ", window devicePixelRatio " << windowRatio << ", 200x120 window grabs "
              << grab.width() << 'x' << grab.height() << " pixels\n";
    expect(same(screenRatio, wanted), "the screen's device pixel ratio is the registered scale");
    expect(same(windowRatio, wanted), "a shown window's device pixel ratio is the registered scale");
    expect(grab.width() == static_cast<int>(std::lround(200 * wanted)) &&
               grab.height() == static_cast<int>(std::lround(120 * wanted)),
           "a grab of the window has the registered scale's pixel size");

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "the process runs at scale " << wanted << '\n';
    return 0;
}
