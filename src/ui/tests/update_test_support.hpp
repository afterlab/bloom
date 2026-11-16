#pragma once

// What the update service's tests share: a web server on this machine inside the test process,
// the fixtures of src/update (signed with RFC 8032's public test keys), and an environment for
// the service.

#include "await_qt.hpp"
#include "qt_update_transport.hpp"

#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/update_service.hpp>
#include <bloom/update/keys.hpp>

#include <QByteArray>
#include <QCoreApplication>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
#include <thread>

#ifndef BLOOM_UPDATE_FIXTURES
#error "BLOOM_UPDATE_FIXTURES must name the fixture directory of src/update"
#endif

namespace bloom::ui::update_tests {

inline int failures = 0;
inline void expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

[[nodiscard]] inline QByteArray fixture(const std::string& name) {
    std::ifstream file(std::filesystem::path(BLOOM_UPDATE_FIXTURES) / name, std::ios::binary);
    const std::string content{std::istreambuf_iterator<char>(file),
                              std::istreambuf_iterator<char>()};
    return QByteArray(content.data(), static_cast<qsizetype>(content.size()));
}

[[nodiscard]] inline QByteArray answer(const QByteArray& body,
                                       const QByteArray& extraHeaders = {}) {
    return "HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) +
           "\r\nConnection: close\r\n" + extraHeaders + "\r\n" + body;
}

// A web server on this machine, on the test's own thread: the jobs under test run on the
// scheduler's threads and are answered while the test waits with the event loop running.
struct Loopback final {
    QTcpServer server;
    // The whole answer for a request, by what was asked.
    std::function<QByteArray(const QByteArray& request)> respond;
    // Every request as it arrived, headers included.
    QList<QByteArray> requests;
    // Send only this many bytes of an answer and then say nothing more (the connection stays).
    qsizetype stallAfter = -1;

    Loopback() {
        (void)server.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            auto* socket = server.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                const QByteArray request = socket->property("request").toByteArray() +
                                           socket->readAll();
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n") || socket->property("answered").toBool()) {
                    return;
                }
                socket->setProperty("answered", true);
                requests.push_back(request);
                const QByteArray response = respond ? respond(request) : QByteArray();
                if (stallAfter >= 0) {
                    socket->write(response.left(stallAfter));
                    socket->flush();
                    return;
                }
                socket->write(response);
                socket->disconnectFromHost();
            });
        });
    }

    [[nodiscard]] std::string origin() const {
        return "http://127.0.0.1:" + std::to_string(server.serverPort());
    }
};

// The header lines of a recorded request, lower case names, without the request line.
[[nodiscard]] inline QList<QByteArray> headerNames(const QByteArray& request) {
    QList<QByteArray> names;
    const auto lines = request.left(request.indexOf("\r\n\r\n")).split('\n');
    for (qsizetype index = 1; index < lines.size(); ++index) {
        names.push_back(lines[index].left(lines[index].indexOf(':')).trimmed().toLower());
    }
    return names;
}

[[nodiscard]] inline QByteArray headerValue(const QByteArray& request, const QByteArray& name) {
    const auto lines = request.left(request.indexOf("\r\n\r\n")).split('\n');
    for (qsizetype index = 1; index < lines.size(); ++index) {
        const auto colon = lines[index].indexOf(':');
        if (lines[index].left(colon).trimmed().toLower() == name) {
            return lines[index].mid(colon + 1).trimmed();
        }
    }
    return {};
}

// Sends every request to the test's server, keeping the last part of the address: the fixture
// feed is signed, so the addresses inside it cannot name a port that is only known now. Also
// notes the thread it was used on.
class LoopbackTransport final : public update::UpdateTransport {
  public:
    LoopbackTransport(std::string origin, std::shared_ptr<std::atomic<bool>> usedOnInterfaceThread)
        : origin_(std::move(origin)), usedOnInterfaceThread_(std::move(usedOnInterfaceThread)) {}

    [[nodiscard]] update::TransportResult get(const update::TransportRequest& request,
                                              const update::TransportSink& sink,
                                              const std::function<bool()>& cancelled,
                                              const update::TransportProgress& progress) override {
        if (QCoreApplication::instance() != nullptr &&
            QThread::currentThread() == QCoreApplication::instance()->thread()) {
            usedOnInterfaceThread_->store(true);
        }
        auto local = request;
        local.url = origin_ + request.url.substr(request.url.find_last_of('/'));
        return transport_.get(local, sink, cancelled, progress);
    }

  private:
    std::string origin_;
    std::shared_ptr<std::atomic<bool>> usedOnInterfaceThread_;
    QtUpdateTransport transport_;
};

// A scheduler, its bridge and a temporary folder for one case.
struct Bench final {
    runtime::TaskScheduler scheduler{{.cpuWorkerCount = 1,
                                      .blockingIoWorkerCount = 1,
                                      .cpuQueueCapacity = 4,
                                      .blockingIoQueueCapacity = 4}};
    TaskUiBridge bridge{scheduler, nullptr, std::chrono::milliseconds(1)};
    std::filesystem::path root;
    std::shared_ptr<std::atomic<bool>> usedOnInterfaceThread =
        std::make_shared<std::atomic<bool>>(false);

    explicit Bench(const std::string& name)
        : root(std::filesystem::temp_directory_path() /
               ("bloom-update-ui-" + name + "-" + std::to_string(std::random_device{}()))) {
        std::filesystem::create_directories(root);
        bridge.start();
    }
    ~Bench() {
        scheduler.beginShutdown();
        (void)test_support::awaitQtCondition("the scheduler ends",
                                             [&] { return scheduler.isQuiescent(); });
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    // An installed copy from the install script (a folder the user owns), version
    // 0.1.0-alpha.1, reading its feed from `origin`.
    [[nodiscard]] UpdateEnvironment environment(const std::string& origin) const {
        UpdateEnvironment result;
        result.configuration = update::makeConfiguration(
            origin + "/alpha.json", fixture("trusted-key.txt").trimmed().toStdString(), {}, true);
        result.current = update::ReleaseVersion::parse("0.1.0-alpha.1").value();
        result.install.kind = update::InstallKind::UserFolder;
        result.install.root = root / "opt" / "bloom";
        result.platformKey = "linux-x86_64";
        result.folders.root = root / "data" / "updates";
        result.processId = 4242;
        result.makeTransport = [origin, used = usedOnInterfaceThread] {
            return std::make_unique<LoopbackTransport>(origin, used);
        };
        return result;
    }
};

} // namespace bloom::ui::update_tests
