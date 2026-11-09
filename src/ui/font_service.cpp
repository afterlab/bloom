#include "font_service.hpp"

#include "qt_font_transport.hpp"

#include <bloom/runtime/font_resolver.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/window_status_bar.hpp>

#include <QDebug>
#include <QMetaObject>

#include <memory>

namespace bloom::ui {

FontServiceBridge::FontServiceBridge(WindowStatusBar* const statusBar,
                                     CompositionPreviewController* const preview,
                                     QObject* const parent)
    : QObject(parent), statusBar_(statusBar), preview_(preview) {
    auto& resolver = runtime::fonts::processFontResolver();
    resolver.setTransport(std::make_shared<QtFontTransport>());
    // Both sinks run on the resolver's worker thread: they only queue a call to this object.
    resolver.setNoticeSink([this](const runtime::fonts::FontNotice& notice) {
        QMetaObject::invokeMethod(this, "showNotice", Qt::QueuedConnection,
                                  Q_ARG(QString, QString::fromStdString(notice.message)));
    });
    resolver.setArrivalSink([this](const bool preview) {
        // A picker row's name-only subset changes no plan; only a face a text node uses does.
        if (!preview)
            QMetaObject::invokeMethod(this, "fontArrived", Qt::QueuedConnection);
    });
}

FontServiceBridge::~FontServiceBridge() {
    auto& resolver = runtime::fonts::processFontResolver();
    resolver.setNoticeSink({});
    resolver.setArrivalSink({});
}

void FontServiceBridge::showNotice(const QString& message) {
    // Transient only: the status bar and the log. The persistent sign is the red font name.
    qInfo().noquote() << "Fonts:" << message;
    if (statusBar_ != nullptr)
        statusBar_->showTransientMessage(message);
}

void FontServiceBridge::fontArrived() {
    if (preview_ == nullptr)
        return;
    // Frames cached with the fallback are stale now: drop them and draw the real font.
    preview_->purgePreviewCache();
    preview_->requestRefresh();
}

} // namespace bloom::ui
