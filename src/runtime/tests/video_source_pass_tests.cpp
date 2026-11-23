// #772: a linear pass (an export) admits every video source its frames read at once, beyond the
// sources an idle context keeps (VideoSourceContext::kSnapshotFiles), and the source-snapshot
// file quota follows them. A source an evaluated frame no longer reads has left the range: its
// session (and with it its snapshot) is released then, whatever order frames finish in. When the
// pass ends, the context keeps no more than its idle retention. Session construction launches no
// worker, so this checks admission and release only.
#include "video_source.hpp"

#include <bloom/core/rational_time.hpp>
#include <bloom/runtime/operation_cache.hpp>

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

namespace {
using namespace bloom;
using runtime::detail::VideoSourceContext;

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

core::RationalTime at(const std::int64_t frame) { return *core::RationalTime::create(frame, 24); }

void passAdmitsAndReleases() {
    const auto context = std::make_shared<VideoSourceContext>();
    context->setSessionByteBudget(std::size_t{1} << 50U);
    const auto path = [](const int source) {
        return std::filesystem::path("movie-layers") / ("layer-" + std::to_string(source) + ".mov");
    };
    const auto key = [](const int source) { return "layer-" + std::to_string(source); };
    auto pass = std::make_shared<runtime::LinearPassRetention>();
    const auto read = [&](const int source, const std::int64_t frame) {
        return context->session(key(source), path(source), true, 1,
                                {.pass = pass, .passFrame = at(frame)}) != nullptr;
    };
    // Frame 0 reads sixteen movie layers at once: every one is admitted.
    bool opened = true;
    for (int source = 0; source < 16; ++source)
        opened = read(source, 0) && opened;
    expect(opened, "sixteen sessions open");
    expect(context->retainedSessionCount() == 16,
           "a pass keeps every source its frame reads (" +
               std::to_string(context->retainedSessionCount()) + " kept)");
    expect(context->sourceSnapshotFileLimit() == VideoSourceContext::kSnapshotFiles + 16,
           "the snapshot file quota follows the sources the pass needs (" +
               std::to_string(context->sourceSnapshotFileLimit()) + ")");
    // Frames 1 and 2 are in flight together. Frame 2 reads layers 0..9; frame 1 reads 0..11.
    for (int source = 0; source < 10; ++source)
        static_cast<void>(read(source, 2));
    for (int source = 0; source < 12; ++source)
        static_cast<void>(read(source, 1));
    // Frame 2 finishes first: layers 10 and 11 were last read for frame 1 (still before frame 2),
    // and 12..15 for frame 0. None is read at frame 2: they left the range and are released.
    pass->frameEvaluated(at(2));
    expect(context->retainedSessionCount() == 10,
           "the sources an evaluated frame no longer reads are released (" +
               std::to_string(context->retainedSessionCount()) + " kept)");
    expect(context->sourceSnapshotFileLimit() == VideoSourceContext::kSnapshotFiles + 10,
           "the quota follows the sources still needed");
    // Frame 1 finishing late changes nothing: every kept source was read for frame 2.
    pass->frameEvaluated(at(1));
    expect(context->retainedSessionCount() == 10, "a frame finishing out of order keeps the rest");
    // A layer that enters later is admitted beside the others.
    for (int source = 0; source < 10; ++source)
        static_cast<void>(read(source, 3));
    static_cast<void>(read(20, 3));
    pass->frameEvaluated(at(3));
    expect(context->retainedSessionCount() == 11, "a source entering the range is admitted");
    // A capture that waited for released snapshots is noted for the pass's owner to report.
    pass->noteVideoSnapshotWait(
        {.frame = at(4), .source = "layer-3", .waits = 2, .nanoseconds = 40'000'000});
    const auto waits = pass->takeVideoSnapshotWaits();
    expect(waits.size() == 1 && waits.front().source == "layer-3" && waits.front().waits == 2 &&
               waits.front().frame == at(4),
           "a snapshot wait is noted with its frame and source");
    expect(pass->takeVideoSnapshotWaits().empty(), "a noted wait is reported once");
    // The pass ends: the context keeps its idle retention only.
    pass.reset();
    expect(context->retainedSessionCount() == VideoSourceContext::kSnapshotFiles,
           "after the pass the idle retention bounds what is kept (" +
               std::to_string(context->retainedSessionCount()) + " kept)");
    expect(context->sourceSnapshotFileLimit() == VideoSourceContext::kSnapshotFiles,
           "after the pass the quota is the idle retention's");
    // Without a pass (preview, thumbnails) the idle retention bounds sources, as before.
    for (int source = 30; source < 50; ++source)
        static_cast<void>(context->session(key(source), path(source), true, 1));
    expect(context->retainedSessionCount() == VideoSourceContext::kSnapshotFiles,
           "requests outside a pass keep the idle retention");
    context->beginShutdown();
}
} // namespace

int main() {
    passAdmitsAndReleases();
    if (failures == 0)
        std::cout << "video source pass admission: ok\n";
    return failures == 0 ? 0 : 1;
}
