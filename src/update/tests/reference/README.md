# Reference files of the update tests

These files were made by other programs than Bloom's, and the tests compare Bloom's output with
them. They are not made again by `tools/release/make_update_fixtures.py`.

## `zsync-0.6.6.zsync`

The control file the reference tool writes for the test's release file:

    zsyncmake -o zsync-0.6.6.zsync -u Bloom-new.AppImage -f Bloom-new.AppImage new.bin

made with `zsyncmake` 0.6.6. `new.bin` is 300,000 bytes: the SHA-256 digests of the texts
`bloom zsync fixture 0` to `bloom zsync fixture 9374`, one after the other (the update tests and
the fixture generator both make it that way). Its `MTime` line is the time of that file when the
tool ran. `bloom.update.zsync_tests` requires Bloom's own writer to give the same bytes.
