// The stdio transport (newline-delimited UTF-8 JSON-RPC), moved from apps/bloom-mcp/transport.cpp:
// a bounded reader thread, the 16-message inbox, cancellation intercepted while a call runs, and
// one protocol session. Cancellation now stops only the cancelled request's own work.
#include <bloom/mcp/server_core.hpp>

#include <condition_variable>
#include <deque>
#include <istream>
#include <memory>
#include <mutex>
#include <ostream>
#include <stop_token>
#include <thread>

namespace bloom::mcp {
namespace {

struct Message {
    std::string text;
    bool oversized = false;
    bool unterminated = false;
};

// Shared with the reader thread, which may outlive serveStdio() (see below).
struct Inbox {
    std::mutex mutex;
    std::condition_variable_any ready;
    std::condition_variable space;
    std::deque<Message> queue;
    // The reader reached end of input.
    bool closed = false;
    // serveStdio() returned; the reader drops what it still reads.
    bool abandoned = false;
    // The request handle() is running, and its stop source.
    std::optional<std::string> active;
    std::stop_source cancel;
};

} // namespace

void serveStdio(McpServerCore& core, std::istream& input, std::ostream& output,
                const std::stop_token& stop) {
    const auto limits = kMcpMessageLimits;
    const auto shared = std::make_shared<Inbox>();
    auto& inbox = *shared;
    auto session = core.openSession();
    // The reader touches only the shared inbox and `input`, never the core, the session or
    // `output`, so it can be left blocked on idle input when `stop` fires.
    std::thread reader([shared, &input, limits] {
        auto& box = *shared;
        const auto send = [&](Message message) {
            std::unique_lock lock(box.mutex);
            box.space.wait(
                lock, [&] { return box.abandoned || box.queue.size() < limits.pendingMessages; });
            if (box.abandoned)
                return;
            box.queue.push_back(std::move(message));
            box.ready.notify_one();
        };
        // notifications/cancelled is handled here, while the call it cancels may be running.
        const auto intercept = [&](const std::string& text) {
            const auto envelope = inspectMessage(text, limits);
            if (envelope.kind != McpMessageKind::Notification ||
                envelope.method != "notifications/cancelled")
                return false;
            const std::lock_guard lock(box.mutex);
            if (envelope.cancels && box.active == envelope.cancels)
                box.cancel.request_stop();
            return true;
        };
        Message message;
        char byte = 0;
        while (input.get(byte)) {
            if (byte == '\n') {
                if (message.oversized || !intercept(message.text))
                    send(std::move(message));
                message = {};
            } else if (message.text.size() < limits.requestBytes) {
                message.text.push_back(byte);
            } else {
                message.oversized = true;
            }
        }
        if (!message.text.empty() || message.oversized) {
            message.unterminated = true;
            send(std::move(message));
        }
        const std::lock_guard lock(box.mutex);
        box.closed = true;
        box.ready.notify_one();
    });
    // On every exit: a reader that reached end of input is joined. One still blocked on idle input
    // cannot be interrupted portably, so it is detached: it holds the inbox and ends at end of
    // input.
    struct Release final {
        Inbox& box;
        std::thread& thread;
        ~Release() {
            bool finished = false;
            {
                const std::lock_guard lock(box.mutex);
                box.abandoned = true;
                finished = box.closed;
                box.space.notify_all();
            }
            if (finished)
                thread.join();
            else
                thread.detach();
        }
    } release{inbox, reader};
    while (!stop.stop_requested()) {
        Message message;
        McpRequestControl control;
        {
            std::unique_lock lock(inbox.mutex);
            if (!inbox.ready.wait(lock, stop, [&] { return inbox.closed || !inbox.queue.empty(); }))
                break; // Stopped while waiting for input.
            if (inbox.queue.empty())
                break;
            message = std::move(inbox.queue.front());
            inbox.queue.pop_front();
            inbox.space.notify_one();
            if (!message.oversized && !message.unterminated) {
                inbox.cancel = std::stop_source();
                inbox.active = inspectMessage(message.text, limits).requestKey;
                control.stop = inbox.cancel.get_token();
            }
        }
        std::optional<std::string> response;
        if (message.oversized)
            response = McpServerCore::errorResponse(std::nullopt, McpProtocolError::InvalidRequest,
                                                    "Request exceeds 1 MiB");
        else if (message.unterminated)
            response = McpServerCore::errorResponse(std::nullopt, McpProtocolError::ParseError,
                                                    "Unterminated stdio message");
        else
            response = core.handle(*session, message.text, control).response;
        {
            const std::lock_guard lock(inbox.mutex);
            inbox.active.reset();
        }
        if (response)
            output << *response << '\n' << std::flush;
    }
}

} // namespace bloom::mcp
