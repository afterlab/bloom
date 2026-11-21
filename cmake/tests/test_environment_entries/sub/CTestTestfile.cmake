# A subdirectory's CTest file, for bloom.quality.test_environment_entries_rejects.
add_test("fixture.sub-split" "/bin/true")
set_tests_properties("fixture.sub-split" PROPERTIES  ENVIRONMENT [[QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=2]] LABELS "ui" TIMEOUT "60")
add_test("fixture.sub-joined.dpr2" "/bin/true")
set_tests_properties("fixture.sub-joined.dpr2" PROPERTIES  ENVIRONMENT [[QT_QPA_PLATFORM=offscreen\;QT_SCALE_FACTOR=2]] LABELS "ui" TIMEOUT "60")
