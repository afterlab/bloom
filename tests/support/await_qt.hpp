#pragma once

// await.hpp for tests that own a Qt event loop: the wait delivers queued events between polls, so
// work that finishes on the UI thread can complete. See await.hpp for the contract.

#include "await.hpp"

#include <QCoreApplication>
#include <QEventLoop>

namespace bloom::test_support {

template <typename Predicate>
[[nodiscard]] bool awaitQtCondition(const std::string_view name, Predicate&& predicate,
                                    const std::chrono::milliseconds limit = kAwaitLimit) {
    return awaitConditionWith(
        name, std::forward<Predicate>(predicate),
        [] {
            QCoreApplication::sendPostedEvents();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        },
        limit);
}

} // namespace bloom::test_support
