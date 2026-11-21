// bloom.runtime.font_downloads_exit (#750): a program that ends while a font download is running
// ends it before its exit handlers run.
//
// The MCP server once hung for ever after its client closed its input: a font download was in its
// TLS handshake when main() returned, OpenSSL's exit handler (registered when libcurl was first
// loaded, after the process font resolver was made) tore down OpenSSL's locks under it, the
// transfer spun on a destroyed lock, and the resolver's exit-time destructor waited for it.
//
// This program is such a composition root. Its transport stands for a library like that: on first
// use it registers an exit handler that tears the library down, and that handler fails the program
// when a transfer is still running. main() holds the downloads' owner, starts a download, waits
// until its transfer runs and returns. The owner must end the transfer before main() returns.
#include <bloom/document/font_id.hpp>
#include <bloom/runtime/font_resolver.hpp>
#include <bloom/runtime/headless_font_transport.hpp>

#include "await.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

using namespace bloom::runtime::fonts;

std::atomic<bool> transferRunning{false};
std::atomic<bool> transferStarted{false};

// The library's exit-time teardown: what OpenSSL's exit handler is to a libcurl transfer.
void tearDownLibrary() {
    if (transferRunning.load()) {
        std::fputs("FAIL: the transport's library was torn down by an exit handler while a "
                   "download was still running\n",
                   stderr);
        std::fflush(stderr);
        std::_Exit(3);
    }
}

class ExitTornTransport final : public FontTransport {
  public:
    [[nodiscard]] FontHttpResult
    get(const std::string&, std::uint64_t, const std::function<bool()>& cancelled,
        const std::function<void(std::uint64_t, std::uint64_t)>&) override {
        // Loaded when first needed, as libcurl is: its exit handler comes after the resolver's.
        static const bool registered = std::atexit(&tearDownLibrary) == 0;
        if (!registered)
            std::cerr << "FAIL: the exit handler could not be registered\n";
        transferRunning = true;
        transferStarted = true;
        // A transfer that waits on its server and checks for cancellation as it does.
        while (!(cancelled && cancelled()))
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        FontHttpResult result;
        result.status = FontHttpStatus::Cancelled;
        transferRunning = false;
        return result;
    }
};

} // namespace

int main() {
    // The process resolver, as the MCP server, bloom-cli and Python use it.
    auto& resolver = processFontResolver();
    {
        const HeadlessFontDownloads downloads(resolver, std::make_shared<ExitTornTransport>());
        // A Google face no cache can hold: it always downloads.
        bloom::document::FontId id;
        id.source = bloom::document::FontSource::Google;
        id.family = "Bloom Exit Test Family";
        id.style = "Bold";
        id.weight = 700;
        resolver.prefetch(id);
        if (!bloom::test_support::awaitCondition("the download's transfer runs",
                                                 [] { return transferStarted.load(); }))
            return 1;
    }
    // main() returns and the exit handlers run: tearDownLibrary() fails the program if the
    // transfer is still running.
    std::cout << "the downloads' owner has ended; main() returns\n";
    return bloom::test_support::awaitFailureCount() == 0 ? 0 : 1;
}
