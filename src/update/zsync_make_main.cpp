// bloom-zsync-make (ADR 0041): writes the zsync control file a release publishes beside its
// AppImage, from which an installed AppImage downloads only the blocks it lacks. A build tool
// of the release: it is not part of any package.
//
//   bloom-zsync-make <file> <published name> <control file> <time>
//
// <published name> is the file's name as it is published beside the control file (the control
// file's Filename and URL lines), and <time> the file's time as seconds since 1970 (its MTime
// line), given so that the same file always gives the same control file.
#include <bloom/update/zsync.hpp>

#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string_view>

int main(const int argc, char* argv[]) {
    if (argc != 5) {
        std::fputs("usage: bloom-zsync-make <file> <published name> <control file> <time>\n",
                   stderr);
        return 2;
    }
    bloom::update::ZsyncControlHeader header;
    header.fileName = argv[2];
    const std::string_view time(argv[4]);
    const auto parsed =
        std::from_chars(time.data(), time.data() + time.size(), header.modifiedSeconds);
    if (parsed.ec != std::errc{} || parsed.ptr != time.data() + time.size() ||
        header.fileName.empty() || header.fileName.find_first_of("\r\n") != std::string::npos) {
        std::fputs("bloom-zsync-make: the name or the time is not usable\n", stderr);
        return 2;
    }
    const auto control = bloom::update::makeZsyncControl(argv[1], header);
    if (!control.has_value()) {
        std::fprintf(stderr, "bloom-zsync-make: %s cannot be read\n", argv[1]);
        return 1;
    }
    std::ofstream out(argv[3], std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(control->data()),
              static_cast<std::streamsize>(control->size()));
    out.flush();
    if (!out) {
        std::fprintf(stderr, "bloom-zsync-make: %s cannot be written\n", argv[3]);
        return 1;
    }
    return 0;
}
