#include "qt_font_transport.hpp"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <string>

namespace bloom::ui {

namespace {
// The server must answer within this long once connected; a stalled transfer fails instead of
// holding the font worker forever.
constexpr int kTransferTimeoutMs = 20000;
constexpr int kCancelPollMs = 40;
// How much of the body Qt may hold before this class takes it: the body is never buffered whole
// inside Qt, whatever the server sends.
constexpr qint64 kReadBufferBytes = 64 * 1024;

[[nodiscard]] std::string tooLargeText(const std::uint64_t maximumBytes) {
    const auto megabytes = (maximumBytes + (1U << 20U) - 1U) >> 20U;
    return "the server sent more than " +
           (maximumBytes >= (1U << 20U) ? std::to_string(megabytes) + " MB"
                                        : std::to_string(maximumBytes) + " bytes") +
           ", which is more than this download can be, so it was stopped";
}

[[nodiscard]] bool looksOffline(const QNetworkReply::NetworkError error) {
    switch (error) {
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::TimeoutError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::UnknownNetworkError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::ProxyConnectionRefusedError:
    case QNetworkReply::ProxyNotFoundError:
    case QNetworkReply::ProxyTimeoutError:
        return true;
    default:
        return false;
    }
}
} // namespace

runtime::fonts::FontHttpResult
QtFontTransport::get(const std::string& url, const std::uint64_t maximumBytes,
                     const std::function<bool()>& cancelled,
                     const std::function<void(std::uint64_t, std::uint64_t)>& progress) {
    using runtime::fonts::FontHttpStatus;
    runtime::fonts::FontHttpResult result;
    // The manager, the reply and the loop all live on the calling (worker) thread.
    QNetworkAccessManager manager;
    QNetworkRequest request{QUrl(QString::fromStdString(url))};
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Bloom"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(kTransferTimeoutMs);
    // The bytes on the wire are the bytes counted: Qt does not expand a compressed response,
    // which could grow past the cap after it was received. Font files are compressed formats
    // already.
    request.setRawHeader("Accept-Encoding", "identity");
    QNetworkReply* reply = manager.get(request);
    reply->setReadBufferSize(kReadBufferBytes);

    QEventLoop loop;
    bool wasCancelled = false;
    bool tooLarge = false;
    bool tookTooLong = false;
    QByteArray body;
    // The body is taken as it arrives and the download is cut off as soon as it passes the cap,
    // whether or not the server announced a length.
    const auto take = [&] {
        if (tooLarge)
            return;
        body += reply->readAll();
        if (static_cast<std::uint64_t>(body.size()) > maximumBytes) {
            tooLarge = true;
            body.clear();
            reply->abort();
        }
    };
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, take);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(
        reply, &QNetworkReply::downloadProgress, &loop,
        [&](const qint64 received, const qint64 total) {
            if (static_cast<std::uint64_t>(std::max<qint64>(received, 0)) > maximumBytes ||
                static_cast<std::uint64_t>(std::max<qint64>(total, 0)) > maximumBytes) {
                tooLarge = true;
                reply->abort();
                return;
            }
            if (progress)
                progress(static_cast<std::uint64_t>(std::max<qint64>(received, 0)),
                         static_cast<std::uint64_t>(std::max<qint64>(total, 0)));
        });
    QElapsedTimer clock;
    clock.start();
    QTimer poll;
    poll.setInterval(kCancelPollMs);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (cancelled && cancelled()) {
            wasCancelled = true;
            reply->abort();
        } else if (clock.elapsed() > totalTimeoutMs_) {
            tookTooLong = true;
            reply->abort();
        }
    });
    poll.start();
    if (!reply->isFinished())
        loop.exec();
    poll.stop();

    const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.httpStatus = http;
    const auto error = reply->error();
    if (wasCancelled) {
        result.status = FontHttpStatus::Cancelled;
    } else if (tooLarge) {
        result.status = FontHttpStatus::TooLarge;
        result.error = tooLargeText(maximumBytes);
    } else if (tookTooLong) {
        result.status = FontHttpStatus::Failed;
        result.error = "the download took longer than " + std::to_string(totalTimeoutMs_ / 1000) +
                       " seconds, so it was stopped";
    } else if (error == QNetworkReply::NoError && http >= 200 && http < 300) {
        body += reply->readAll();
        if (static_cast<std::uint64_t>(body.size()) > maximumBytes) {
            result.status = FontHttpStatus::TooLarge;
            result.error = tooLargeText(maximumBytes);
        } else {
            result.status = FontHttpStatus::Ok;
            result.body.assign(reinterpret_cast<const std::uint8_t*>(body.constData()),
                               reinterpret_cast<const std::uint8_t*>(body.constData()) +
                                   body.size());
        }
    } else if (http >= 400) {
        result.status = FontHttpStatus::HttpError;
        result.error = reply->errorString().toStdString();
    } else if (looksOffline(error)) {
        result.status = FontHttpStatus::Offline;
        result.error = reply->errorString().toStdString();
    } else {
        result.status = FontHttpStatus::Failed;
        result.error = reply->errorString().toStdString();
    }
    delete reply;
    return result;
}

} // namespace bloom::ui
