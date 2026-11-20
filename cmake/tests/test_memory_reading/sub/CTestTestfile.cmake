# A subdirectory's CTest file, reached through subdirs() as ctest reaches it.
add_test("fixture.sub-fixed" "/bin/true")
set_tests_properties("fixture.sub-fixed" PROPERTIES  ENVIRONMENT "BLOOM_TEST_FIXED_MEMORY_READING=1,1,1" LABELS "native" TIMEOUT "30")
add_test("fixture.sub-unstated" "/bin/true")
set_tests_properties("fixture.sub-unstated" PROPERTIES  ENVIRONMENT "BLOOM_TEST_CRASH_DIRECTORY=/x" LABELS "ui;integration" TIMEOUT "30")
