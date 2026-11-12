// NETSHARE-1: unit coverage for src/ui/network_share_paths.{hpp,cpp} (the URL <-> local path
// mapping, the mounted-share listing, and the file-dialog sidebar helper) plus an end-to-end
// AssetTree drop test driven through an injected fake GVFS root, so this never depends on a real
// mount being present on the machine running the test.
#include "assets_editor_internal.hpp"
#include "network_share_paths.hpp"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QImage>
#include <QMimeData>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QUrl>

#include <bloom/document/new_project.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QStandardPaths>

#include <iostream>
#include <stdexcept>

namespace {
using namespace bloom;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

bool drop(QWidget& target, const QMimeData& mime) {
    QDragEnterEvent enter(QPoint(50, 50), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&target, &enter);
    if (!enter.isAccepted())
        return false;
    QDropEvent event(QPointF(50, 50), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&target, &event);
    return event.isAccepted();
}

// A fake GVFS root standing in for `$XDG_RUNTIME_DIR/gvfs`, carrying the two mount shapes
// deliverable 5 asks for: a plain `server=/share=` mount, and one with `domain=`/`user=` fields
// interleaved in a different order and a host that differs in case from what a drop URL uses.
struct FakeGvfsRoot {
    QTemporaryDir root;
    QString productionMount;
    QString archiveMount;

    FakeGvfsRoot() {
        require(root.isValid(), "fake gvfs root directory created");
        QDir base(root.path());
        require(base.mkpath(QStringLiteral("smb-share:server=10.10.10.20,share=production")),
                "production mount directory created");
        require(base.mkpath(QStringLiteral(
                    "smb-share:domain=WORKGROUP,server=FileServer,share=Archive,user=alice")),
                "archive mount directory created (reordered domain/server/share/user fields)");
        // Not an smb-share mount: mountedNetworkShares()/localPathForUrl() must ignore it.
        require(base.mkpath(QStringLiteral("Trash")), "unrelated gvfs entry created");
        productionMount =
            base.filePath(QStringLiteral("smb-share:server=10.10.10.20,share=production"));
        archiveMount = base.filePath(QStringLiteral(
            "smb-share:domain=WORKGROUP,server=FileServer,share=Archive,user=alice"));
    }
    [[nodiscard]] QString rootPath() const { return root.path(); }
};

void mappingTests() {
    const FakeGvfsRoot fake;
    const auto root = fake.rootPath();

    {
        const auto url = QUrl::fromLocalFile(QStringLiteral("/tmp/example.png"));
        const auto local = ui::localPathForUrl(url, root);
        require(local.has_value() && *local == QStringLiteral("/tmp/example.png"),
                "a file:// URL maps directly to its local path");
    }
    {
        // Percent-decoded remainder joined onto the mount directory.
        const QUrl url(QStringLiteral("smb://10.10.10.20/production/sub%20dir/photo.png"));
        const auto local = ui::localPathForUrl(url, root);
        require(local.has_value() &&
                    *local == fake.productionMount + QStringLiteral("/sub dir/photo.png"),
                "an smb:// URL onto a mounted share resolves, joining the percent-decoded "
                "remainder onto the mount path");
    }
    {
        // Host match is case-insensitive; the mount also carries reordered domain=/user= fields.
        const QUrl url(QStringLiteral("smb://fileserver/Archive/reel.png"));
        const auto local = ui::localPathForUrl(url, root);
        require(local.has_value() && *local == fake.archiveMount + QStringLiteral("/reel.png"),
                "an smb:// URL resolves to the archive mount despite host case and reordered "
                "domain=/user= fields");
    }
    {
        // Share matching stays exact; only the host is documented as case-insensitive.
        const QUrl url(QStringLiteral("smb://fileserver/archive/reel.png"));
        require(!ui::localPathForUrl(url, root).has_value(),
                "share-name matching does not also fold case");
    }
    {
        const QUrl url(QStringLiteral("smb://10.10.10.20/temporary/x.png"));
        require(!ui::localPathForUrl(url, root).has_value(),
                "an smb:// URL naming an unmounted share resolves to nullopt");
    }
    {
        const QUrl url(QStringLiteral("http://example.com/photo.png"));
        require(!ui::localPathForUrl(url, root).has_value(),
                "a non-file/smb scheme resolves to nullopt");
    }
    {
        const QUrl url(QStringLiteral("smb://10.10.10.20/production/x.png"));
        require(!ui::localPathForUrl(url, QString()).has_value(),
                "an empty gvfsRoot (no GVFS on this platform) never resolves an smb:// URL");
    }
}

void mountedSharesTest() {
    const FakeGvfsRoot fake;
    const auto shares = ui::mountedNetworkShares(fake.rootPath());
    require(shares.size() == 2, "exactly the two smb-share mounts are listed");
    QSet<QString> paths;
    for (const auto& url : shares) {
        require(url.isLocalFile(), "each mounted share is a file:// URL");
        paths.insert(url.toLocalFile());
    }
    require(paths.contains(fake.productionMount) && paths.contains(fake.archiveMount),
            "both mounts are present, and the unrelated directory is not");
    require(ui::mountedNetworkShares(QString()).isEmpty(), "an empty gvfsRoot lists no shares");
    require(ui::mountedNetworkShares(fake.rootPath() + QStringLiteral("/does-not-exist")).isEmpty(),
            "a nonexistent root lists no shares");
}

void shareLabelTests() {
    require(ui::smbShareLabel(QUrl(QStringLiteral("smb://10.10.10.20/temporary/x.png"))) ==
                QStringLiteral("10.10.10.20/temporary"),
            "smbShareLabel names host/share even when the share is not mounted");
    require(!ui::smbShareLabel(QUrl(QStringLiteral("smb://10.10.10.20/"))).has_value(),
            "smbShareLabel needs a share segment");
    require(!ui::smbShareLabel(QUrl(QStringLiteral("file:///tmp/x.png"))).has_value(),
            "smbShareLabel only names smb:// URLs");
    require(ui::networkShareLabel(QStringLiteral(
                "smb-share:domain=WORKGROUP,server=10.10.10.20,share=archive,user=alice")) ==
                QStringLiteral("10.10.10.20/archive"),
            "a mount directory reads as host/share, whatever other fields it carries");
    require(!ui::networkShareLabel(QStringLiteral("renders")).has_value(),
            "an ordinary folder name is not a share");
}

void sidebarTest() {
    const FakeGvfsRoot fake;
    // Sidebar URLs are meaningless to a native platform dialog (Qt ignores them there, and
    // configureFileDialogSidebar() is documented as a no-op for one); force the Qt widget dialog
    // so this test exercises the path the helper actually affects.
    QFileDialog dialog;
    dialog.setOption(QFileDialog::DontUseNativeDialog, true);
    // #533: start from a known sidebar, not whatever the machine's saved dialog settings hold (a
    // real mounted share saved there is not under the fake root, so the helper drops it).
    dialog.setSidebarUrls({QUrl::fromLocalFile(QDir::homePath())});
    const auto before = dialog.sidebarUrls();
    require(before.size() == 1, "the sidebar starts from the test's own place");
    ui::configureFileDialogSidebar(dialog, fake.rootPath());
    const auto after = dialog.sidebarUrls();
    require(after.size() == before.size() + 2, "the sidebar gains exactly the two mounted shares");
    for (const auto& url : before)
        require(after.contains(url), "the dialog's existing sidebar entries survive");
    for (const auto& url : ui::mountedNetworkShares(fake.rootPath()))
        require(after.contains(url), "every mounted share reaches the sidebar");
    ui::configureFileDialogSidebar(dialog, fake.rootPath());
    require(dialog.sidebarUrls().size() == after.size(),
            "configuring the sidebar twice does not duplicate entries");
    // A share remembered from an earlier run that is not mounted now leaves the sidebar; other
    // places stay.
    auto remembered = dialog.sidebarUrls();
    remembered.push_back(
        QUrl::fromLocalFile(fake.rootPath() + QStringLiteral("/smb-share:server=gone,share=old")));
    dialog.setSidebarUrls(remembered);
    ui::configureFileDialogSidebar(dialog, fake.rootPath());
    require(dialog.sidebarUrls() == after, "an unmounted share leaves the sidebar");
}

// The drop test deliverable 5 asks for: AssetTree resolves an smb:// URL through an injected
// fake root (AssetTree::setGvfsRootForTest()), imports it exactly as a local file drop would,
// reports the documented notice for an unmounted share instead of silently dropping it, and
// still refuses an unrelated URL scheme outright at drag time.
void assetTreeDropTest() {
    const FakeGvfsRoot fake;
    QImage image(4, 4, QImage::Format_RGBA8888);
    image.fill(QColor(10, 20, 30, 255));
    const auto mountedFile = fake.productionMount + QStringLiteral("/network-photo.png");
    require(image.save(mountedFile), "fixture PNG written into the fake mount");

    runtime::TaskScheduler scheduler;
    ui::ProjectHost host(scheduler);
    ui::CompositionSession session(*host.liveDocumentAndStack().first,
                                   *host.liveDocumentAndStack().second, host.lowestCompositionId());
    ui::TaskUiBridge bridge(scheduler);
    ui::AssetController controller(session, host, scheduler, bridge);
    ui::AssetsEditor assets(session);
    assets.show();
    QApplication::processEvents();
    auto* tree = assets.findChild<QTreeWidget*>(QStringLiteral("assetsTree"));
    require(tree != nullptr, "assets tree exists");
    auto* networkTree = dynamic_cast<ui::assets::AssetTree*>(tree);
    require(networkTree != nullptr, "the assets tree is the AssetTree subclass");
    networkTree->setGvfsRootForTest(fake.rootPath());

    QString refused;
    QObject::connect(&session, &ui::CompositionSession::commandRejected, &assets,
                     [&](const QString& message) { refused = message; });

    {
        QMimeData mime;
        mime.setUrls({QUrl(QStringLiteral("smb://10.10.10.20/production/network-photo.png"))});
        require(drop(*tree->viewport(), mime), "an smb:// drop onto a mounted share is accepted");
    }
    // Wait for the import's own state; CTest's timeout bounds a hang, not a wall-clock guess.
    while (controller.busy())
        QTest::qWait(10);
    require(session.snapshot().project().assets().size() == 1,
            "the resolved smb:// drop imported exactly one asset");
    require(refused.isEmpty(), "a resolved drop never raises the unmounted-share notice");

    {
        QMimeData mime;
        mime.setUrls({QUrl(QStringLiteral("smb://10.10.10.20/temporary/missing.png"))});
        require(drop(*tree->viewport(), mime),
                "an smb:// URL naming an unmounted share is still accepted at drag time");
    }
    QApplication::processEvents();
    require(session.snapshot().project().assets().size() == 1,
            "the unmounted-share drop imported nothing");
    require(refused ==
                QStringLiteral("Connect to 10.10.10.20/temporary in your file manager first"),
            "the unmounted share produces the documented notice, not a silent drop");

    {
        QMimeData mime;
        mime.setUrls({QUrl(QStringLiteral("http://example.com/photo.png"))});
        require(!drop(*tree->viewport(), mime),
                "a URL scheme this module does not recognize is refused outright");
    }

    controller.cancel();
    bridge.beginShutdown();
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent())
        QTest::qWait(10);
    require(scheduler.isQuiescent(), "network-share drop test tasks shut down safely");
}

} // namespace

int main(int argc, char** argv) {
    // #533: Qt's widget file dialog reads and saves its sidebar in the user's Qt settings. Run
    // under a private home and configuration directory so neither the developer's saved places
    // nor an earlier run's fake shares reach the test, and the test writes nothing into them.
    const QTemporaryDir home;
    if (!home.isValid()) {
        std::cerr << "a private home directory could not be created\n";
        return 1;
    }
    qputenv("HOME", QFile::encodeName(home.path()));
    qputenv("USERPROFILE", QFile::encodeName(home.path()));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(home.path() + QStringLiteral("/config")));
    qputenv("XDG_DATA_HOME", QFile::encodeName(home.path() + QStringLiteral("/data")));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(home.path() + QStringLiteral("/cache")));
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    // Windows takes the home folder from the account, not from the environment, and Qt keeps the
    // widget file dialog's saved places in the user's registry there, which cannot be redirected.
    // The sidebar case creates that dialog, so it runs only where the home is private; it is a
    // Linux file-manager feature (GVFS mounts) with nothing to configure on Windows.
    const bool privateHome = QDir::homePath().startsWith(home.path());
#if !defined(Q_OS_WIN)
    if (!privateHome) {
        std::cerr << "the test does not run under its private home directory\n";
        return 1;
    }
#endif
    try {
        // The other cases build a stand-in for a GVFS mount folder, whose names carry a colon. A
        // Windows folder cannot be named that way, and Windows has no such mounts: a share there
        // is a UNC path, which a file URL already names.
#if defined(Q_OS_WIN)
        const bool mounts = false;
#else
        const bool mounts = true;
#endif
        shareLabelTests();
        if (!mounts) {
            std::cout << "SKIP: GVFS mount cases: no such mounts on this platform\n";
            return 0;
        }
        mappingTests();
        mountedSharesTest();
        if (privateHome)
            sidebarTest();
        else
            std::cout << "SKIP: sidebar case: the saved file dialog places cannot be made "
                         "private on this platform\n";
        assetTreeDropTest();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
