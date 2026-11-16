// The update check (ADR 0041, #734): what the request says, and the check as a background job
// of the update service, against a web server on this machine inside the test.
#include "update_test_support.hpp"

#include <QCoreApplication>

#include <set>

using namespace bloom;
using namespace bloom::ui::update_tests;
using bloom::test_support::awaitQtCondition;

namespace {

// "Sends nothing but the request": one GET of the address, and no header that says who asks.
void testRequest() {
    Loopback server;
    server.respond = [](const QByteArray&) {
        // A cookie the server tries to set must never come back.
        return answer("{}", "Set-Cookie: visitor=1234; Path=/\r\n");
    };
    ui::QtUpdateTransport transport;
    const auto never = [] { return false; };
    std::string body;
    const auto sink = [&body](const std::span<const std::byte> bytes) {
        body.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return true;
    };
    update::TransportRequest request;
    request.url = server.origin() + "/alpha.json";
    request.maximumBytes = 1024;
    const auto first = transport.get(request, sink, never, {});
    const auto second = transport.get(request, sink, never, {});
    expect(first.status == update::TransportStatus::Ok && first.httpStatus == 200 && !first.partial,
           "request: the feed is fetched");
    expect(second.status == update::TransportStatus::Ok && body == "{}{}",
           "request: the body is handed on as it is");
    expect(server.requests.size() == 2, "request: one request for each fetch");
    for (const auto& recorded : server.requests) {
        const auto line = recorded.left(recorded.indexOf("\r\n"));
        expect(line == "GET /alpha.json HTTP/1.1",
               "request: a plain GET of the address with no query (" + line.toStdString() + ")");
        expect(headerValue(recorded, "user-agent") == "Bloom",
               "request: the user agent is the fixed word Bloom, with no version or system");
        expect(headerValue(recorded, "accept-language") == "*",
               "request: the system's language is not sent");
        expect(headerValue(recorded, "accept-encoding") == "identity",
               "request: the bytes are sent as they are");
        // Nothing else: no cookie (not even the one the server just set), no credential, no
        // referring address, no identifier of this copy or this machine.
        static const std::set<QByteArray> allowed{"host", "user-agent", "accept-language",
                                                  "accept-encoding", "connection"};
        for (const auto& name : headerNames(recorded)) {
            expect(allowed.contains(name),
                   "request: no other header is sent (" + name.toStdString() + ")");
        }
        expect(!recorded.contains("visitor"), "request: a cookie a server set is not sent back");
    }

    // A range of a file, as the delta download asks for.
    server.requests.clear();
    server.respond = [](const QByteArray&) {
        return QByteArray("HTTP/1.1 206 Partial Content\r\nContent-Length: 3\r\n"
                          "Content-Range: bytes 2-4/10\r\nConnection: close\r\n\r\ncde");
    };
    body.clear();
    request.range = update::ByteRange{2, 4};
    const auto ranged = transport.get(request, sink, never, {});
    expect(ranged.status == update::TransportStatus::Ok && ranged.partial && body == "cde",
           "request: a range is asked for and its answer is known as a part");
    expect(server.requests.size() == 1 && headerValue(server.requests.front(), "range") ==
                                              "bytes=2-4",
           "request: the range header names the bytes");

    // An error page is not a body.
    request.range.reset();
    server.respond = [](const QByteArray&) {
        return QByteArray("HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\nConnection: close\r\n\r\n"
                          "not here!");
    };
    body.clear();
    const auto missing = transport.get(request, sink, never, {});
    expect(missing.status == update::TransportStatus::HttpError && missing.httpStatus == 404 &&
               body.empty(),
           "request: an error answer is an error and none of its text is handed on");

    // More than the caller allows is cut off.
    server.respond = [](const QByteArray&) { return answer(QByteArray(4096, 'x')); };
    body.clear();
    const auto large = transport.get(request, sink, never, {});
    expect(large.status == update::TransportStatus::TooLarge && body.size() <= 1024,
           "request: an answer over the limit is cut off");
}

struct Signals final {
    int finished = 0;
    bool automatic = false;
    explicit Signals(ui::UpdateService& service) {
        QObject::connect(&service, &ui::UpdateService::checkFinished, &service,
                         [this](const bool wasAutomatic) {
                             ++finished;
                             automatic = wasAutomatic;
                         });
    }
};

void testCheck() {
    // A newer version in the feed: found, verified, and decided for this kind of install.
    {
        Loopback server;
        server.respond = [](const QByteArray&) { return answer(fixture("feed.json")); };
        Bench bench("found");
        ui::UpdateService service(bench.scheduler, bench.bridge,
                                  bench.environment(server.origin()));
        Signals seen(service);
        expect(service.checkNow(), "check: it starts");
        expect(service.state() == ui::UpdateService::State::Checking && !service.lastCheck(),
               "check: the call returns while the check runs");
        expect(!service.checkNow(), "check: a second check does not start beside it");
        expect(awaitQtCondition("the check finishes", [&] { return seen.finished == 1; }),
               "check: it finishes");
        const auto& check = service.lastCheck();
        expect(check.has_value() && check->status == update::CheckStatus::Decided &&
                   check->feed.has_value() && check->feed->version.text() == "0.2.0-alpha.3",
               "check: the feed is verified and names the newer version");
        expect(check.has_value() && check->decision.action == update::UpdateAction::ReplaceFolder &&
                   check->decision.file.has_value() && check->decision.file->kind == "tar",
               "check: a folder install is offered the package tar");
        expect(check.has_value() && check->feed.has_value() &&
                   check->feed->notes.find("fixture note") != std::string::npos,
               "check: the release notes come with it");
        expect(!seen.automatic && service.state() == ui::UpdateService::State::Idle,
               "check: it was asked for, and the service is idle again");
        expect(!bench.usedOnInterfaceThread->load(),
               "check: the request is made on a job's thread, never the interface thread");
        expect(server.requests.size() == 1 &&
                   server.requests.front().startsWith("GET /alpha.json HTTP/1.1\r\n"),
               "check: one request, for the feed");
    }
    // A feed signed by a key this build does not carry is refused, with its reason.
    {
        Loopback server;
        server.respond = [](const QByteArray&) { return answer(fixture("feed-other-key.json")); };
        Bench bench("refused");
        ui::UpdateService service(bench.scheduler, bench.bridge,
                                  bench.environment(server.origin()));
        Signals seen(service);
        expect(service.checkNow(), "refused: the check starts");
        expect(awaitQtCondition("the check finishes", [&] { return seen.finished == 1; }),
               "refused: it finishes");
        expect(service.lastCheck().has_value() &&
                   service.lastCheck()->status == update::CheckStatus::Refused &&
                   service.lastCheck()->diagnostic.refusal == update::UpdateRefusal::FeedUnknownKey &&
                   !service.lastCheck()->feed.has_value(),
               "refused: a feed by an unknown key is refused and nothing of it is offered");
        expect(!service.startDownload(), "refused: nothing can be downloaded from it");
    }
    // The same version as this one: up to date.
    {
        Loopback server;
        server.respond = [](const QByteArray&) { return answer(fixture("feed.json")); };
        Bench bench("current");
        auto environment = bench.environment(server.origin());
        environment.current = update::ReleaseVersion::parse("0.2.0-alpha.3").value();
        ui::UpdateService service(bench.scheduler, bench.bridge, std::move(environment));
        Signals seen(service);
        expect(service.checkNow(), "current: the check starts");
        expect(awaitQtCondition("the check finishes", [&] { return seen.finished == 1; }),
               "current: it finishes");
        expect(service.lastCheck().has_value() &&
                   service.lastCheck()->decision.action == update::UpdateAction::UpToDate,
               "current: the newest version is told that it is");
    }
    // A build without an address or key contacts nothing.
    {
        Loopback server;
        server.respond = [](const QByteArray&) { return answer(fixture("feed.json")); };
        Bench bench("unconfigured");
        auto environment = bench.environment(server.origin());
        environment.configuration = {};
        ui::UpdateService service(bench.scheduler, bench.bridge, std::move(environment));
        Signals seen(service);
        expect(!service.checkAutomatically({}, 1'000'000),
               "unconfigured: no automatic check is started");
        expect(service.checkNow(), "unconfigured: a check that was asked for answers");
        expect(awaitQtCondition("the check finishes", [&] { return seen.finished == 1; }),
               "unconfigured: it finishes");
        expect(service.lastCheck().has_value() &&
                   service.lastCheck()->status == update::CheckStatus::NotConfigured &&
                   server.requests.isEmpty(),
               "unconfigured: it says so and no request was made");
    }
    // The automatic check: on by default, at most once a day, off when turned off.
    {
        Loopback server;
        server.respond = [](const QByteArray&) { return answer(fixture("feed.json")); };
        Bench bench("automatic");
        ui::UpdateService service(bench.scheduler, bench.bridge,
                                  bench.environment(server.origin()));
        Signals seen(service);
        const std::int64_t now = 1'800'000'000;
        expect(!service.checkAutomatically({.enabled = false, .lastCheckSeconds = {}}, now),
               "automatic: turned off, it does not start");
        expect(!service.checkAutomatically({.enabled = true, .lastCheckSeconds = now - 3600}, now),
               "automatic: an hour after the last check, it does not start");
        expect(server.requests.isEmpty(), "automatic: neither made a request");
        expect(service.checkAutomatically(
                   {.enabled = true,
                    .lastCheckSeconds = now - update::kAutomaticCheckIntervalSeconds},
                   now),
               "automatic: a day after the last check, it starts");
        expect(awaitQtCondition("the check finishes", [&] { return seen.finished == 1; }),
               "automatic: it finishes");
        expect(seen.automatic && server.requests.size() == 1,
               "automatic: the result says nobody asked, and one request was made");
    }
    // A build folder is never offered anything by itself.
    {
        Loopback server;
        Bench bench("development");
        auto environment = bench.environment(server.origin());
        environment.install.kind = update::InstallKind::Development;
        ui::UpdateService service(bench.scheduler, bench.bridge, std::move(environment));
        expect(!service.checkAutomatically({}, 1'800'000'000) && server.requests.isEmpty(),
               "development: a build folder starts no automatic check");
    }
    // No answer: the check can be cancelled, and ends.
    {
        Loopback server;
        server.stallAfter = 0;
        Bench bench("cancelled");
        ui::UpdateService service(bench.scheduler, bench.bridge,
                                  bench.environment(server.origin()));
        Signals seen(service);
        expect(service.checkNow(), "cancelled: the check starts");
        expect(awaitQtCondition("the request arrives", [&] { return server.requests.size() == 1; }),
               "cancelled: the request arrives");
        service.cancel();
        expect(awaitQtCondition("the check ends", [&] { return seen.finished == 1; }),
               "cancelled: the check ends");
        expect(service.lastCheck().has_value() &&
                   service.lastCheck()->status == update::CheckStatus::Cancelled &&
                   service.state() == ui::UpdateService::State::Idle,
               "cancelled: it is reported as cancelled");
    }
    // Nothing listens at the address.
    {
        std::string origin;
        {
            Loopback gone;
            origin = gone.origin();
        }
        Bench bench("unreachable");
        ui::UpdateService service(bench.scheduler, bench.bridge, bench.environment(origin));
        Signals seen(service);
        expect(service.checkNow(), "unreachable: the check starts");
        expect(awaitQtCondition("the check ends", [&] { return seen.finished == 1; }),
               "unreachable: the check ends");
        expect(service.lastCheck().has_value() &&
                   service.lastCheck()->status == update::CheckStatus::Unreachable,
               "unreachable: it says the feed could not be fetched");
    }
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    testRequest();
    testCheck();
    return failures + bloom::test_support::awaitFailureCount() == 0 ? 0 : 1;
}
