# A subdirectory's CTest file, for bloom.quality.test_private_home_rejects.
add_test("fixture.sub-own-home" "/bin/true")
set_tests_properties("fixture.sub-own-home" PROPERTIES  ENVIRONMENT "HOME=/build/test-home/fixture.sub-own-home;CFFIXED_USER_HOME=/build/test-home/fixture.sub-own-home" LABELS "unit" TIMEOUT "30")
add_test("fixture.sub-no-home" "/bin/true")
set_tests_properties("fixture.sub-no-home" PROPERTIES  LABELS "unit" TIMEOUT "30")
