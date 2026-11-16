#include "qt_update_transport.hpp"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <span>
#include <string>

namespace bloom::ui {

namespace {
// The server must keep sending: a transfer with no bytes for this long fails instead of holding
// the job. There is no limit on the whole transfer, which is a large file on a slow line.
constexpr int kStallTimeoutMs = 30000;
constexpr int kCancelPollMs = 40;
// How much of the body Qt may hold before this class hands it on.
constexpr qint64 kReadBufferBytes = 256 * 1024;

[[nodiscard]] bool looksOffline(const QNetworkReply::NetworkError error) {
    switch (error) {
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::TimeoutError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::UnknownNetworkError:
    case QNetworkReply::ProxyConnectionClosedError:
    case QNetworkReply::ProxyConnectionRefusedError:
    case QNetworkReply::ProxyNotFoundError:
    case QNetworkReply::ProxyTimeoutError:
        return true;
    default:
        return false;
    }
}
} // namespace

update::TransportResult QtUpdateTransport::get(const update::TransportRequest& request,
                                               const update::TransportSink& sink,
                                               const std::function<bool()>& cancelled,
                                               const update::TransportProgress& progress) {
    using update::TransportStatus;
    update::TransportResult result;
    // The manager, the reply and the loop all live on the calling (worker) thread. A manager
    // made for one request has no cookie and no connection from an earlier one.
    QNetworkAccessManager manager;
    QNetworkRequest networkRequest{QUrl(QString::fromStdString(request.url))};
    networkRequest.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Bloom"));
    networkRequest.setRawHeader("Accept-Language", "*");
    // The bytes on the wire are the bytes counted and checked.
    networkRequest.setRawHeader("Accept-Encoding", "identity");
    networkRequest.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
                                QNetworkRequest::Manual);
    networkRequest.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
                                QNetworkRequest::Manual);
    networkRequest.setAttribute(QNetworkRequest::AuthenticationReuseAttribute,
                                QNetworkRequest::Manual);
    // A release file is served through a redirect to where it is stored; a redirect never leads
    // from HTTPS to plain HTTP.
    networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                QNetworkRequest::NoLessSafeRedirectPolicy);
    networkRequest.setTransferTimeout(kStallTimeoutMs);
    if (request.range.has_value()) {
        networkRequest.setRawHeader("Range", "bytes=" + QByteArray::number(request.range->first) +
                                                 "-" + QByteArray::number(request.range->last));
    }
    QNetworkReply* reply = manager.get(networkRequest);
    reply->setReadBufferSize(kReadBufferBytes);

    QEventLoop loop;
    bool wasCancelled = false;
    bool tooLarge = false;
    bool refused = false;
    std::uint64_t received = 0;
    // Only the body of an answer that succeeded is handed on: the text of an error page is not
    // part of any file.
    const auto take = [&] {
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (tooLarge || refused || http < 200 || http >= 300) {
            return;
        }
        const QByteArray bytes = reply->readAll();
        if (bytes.isEmpty()) {
            return;
        }
        received += static_cast<std::uint64_t>(bytes.size());
        if (received > request.maximumBytes) {
            tooLarge = true;
            reply->abort();
            return;
        }
        if (sink && !sink(std::as_bytes(std::span(bytes.constData(),
                                                  static_cast<std::size_t>(bytes.size()))))) {
            refused = true;
            reply->abort();
            return;
        }
        if (progress) {
            const auto announced = reply->header(QNetworkRequest::ContentLengthHeader);
            progress(received, announced.isValid() ? announced.toULongLong() : 0U);
        }
    };
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, take);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer poll;
    poll.setInterval(kCancelPollMs);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (cancelled && cancelled()) {
            wasCancelled = true;
            reply->abort();
        }
    });
    poll.start();
    if (!reply->isFinished()) {
        loop.exec();
    }
    poll.stop();

    const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.httpStatus = http;
    result.partial = http == 206;
    const auto error = reply->error();
    if (wasCancelled) {
        result.status = TransportStatus::Cancelled;
    } else if (tooLarge) {
        result.status = TransportStatus::TooLarge;
        result.error = "the server sent more than this download can be";
    } else if (refused) {
        result.status = TransportStatus::Failed;
        result.error = "the download was stopped";
    } else if (error == QNetworkReply::NoError && http >= 200 && http < 300) {
        take();
        result.status = tooLarge  ? TransportStatus::TooLarge
                        : refused ? TransportStatus::Failed
                                  : TransportStatus::Ok;
    } else if (http >= 400) {
        result.status = TransportStatus::HttpError;
        result.error = reply->errorString().toStdString();
    } else if (looksOffline(error)) {
        result.status = TransportStatus::Offline;
        result.error = reply->errorString().toStdString();
    } else {
        result.status = TransportStatus::Failed;
        result.error = reply->errorString().toStdString();
    }
    delete reply;
    return result;
}

} // namespace bloom::ui
