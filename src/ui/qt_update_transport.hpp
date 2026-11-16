#pragma once

// The desktop application's network transport for updates (ADR 0041): one plain GET over Qt's
// secure transport, run on the worker thread of the job that asks, cancellable, with a stall
// timeout. The request says nothing about who asks: no cookie is sent or kept, no stored
// credential is reused, the user agent is the fixed word "Bloom", and the language header Qt
// would fill from the system's locale is set to "any". The body is handed on in small pieces as
// it arrives, so a release file is never held in memory, and the transfer is cut off as soon as
// it passes the size the caller allows.

#include <bloom/update/transport.hpp>

namespace bloom::ui {

class QtUpdateTransport final : public update::UpdateTransport {
  public:
    [[nodiscard]] update::TransportResult get(const update::TransportRequest& request,
                                              const update::TransportSink& sink,
                                              const std::function<bool()>& cancelled,
                                              const update::TransportProgress& progress) override;
};

} // namespace bloom::ui
