#pragma once

// Connects the process font resolver (#489) to the desktop window: installs the Qt network
// transport, shows its transient notices in the status bar and the log, and refreshes the Viewer
// when a font that stood in as a fallback arrives.

#include <QObject>
#include <QString>

namespace bloom::ui {

class CompositionPreviewController;
class WindowStatusBar;

class FontServiceBridge final : public QObject {
    Q_OBJECT

  public:
    FontServiceBridge(WindowStatusBar* statusBar, CompositionPreviewController* preview,
                      QObject* parent);
    ~FontServiceBridge() override;

  private Q_SLOTS:
    void showNotice(const QString& message);
    void fontArrived();

  private:
    WindowStatusBar* statusBar_ = nullptr;
    CompositionPreviewController* preview_ = nullptr;
};

} // namespace bloom::ui
