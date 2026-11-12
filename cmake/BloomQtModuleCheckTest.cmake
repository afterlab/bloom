# Tests BloomQtModuleCheck.cmake on synthetic trees (the bloom.quality.qt_modules_self_test test).
#
#   cmake -DBLOOM_WORK=<empty scratch directory> -P BloomQtModuleCheckTest.cmake
#
# The check must pass a build that names only listed LGPL parts, and must fail, with the reason,
# on a GPL-only module added to the build, on a GPL-only module added to the list, on a part
# offered under a commercial license only, and on a listed part the locked Qt does not have.
cmake_minimum_required(VERSION 3.25)
if(NOT BLOOM_WORK)
    message(FATAL_ERROR "BloomQtModuleCheckTest.cmake needs BLOOM_WORK")
endif()
set(check "${CMAKE_CURRENT_LIST_DIR}/BloomQtModuleCheck.cmake")
set(lgpl "LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only")
set(gpl "LicenseRef-Qt-Commercial OR GPL-3.0-only")

function(write_tree build_text list_text)
    file(REMOVE_RECURSE "${BLOOM_WORK}")
    file(MAKE_DIRECTORY "${BLOOM_WORK}/source/src" "${BLOOM_WORK}/qt/sbom")
    file(WRITE "${BLOOM_WORK}/source/CMakeLists.txt"
         "bloom_find_locked_qt(Network Widgets)\n# Qt6::Unlisted is only named by a comment.\n")
    file(WRITE "${BLOOM_WORK}/source/src/CMakeLists.txt" "${build_text}")
    file(WRITE "${BLOOM_WORK}/list.txt" "${list_text}")
    set(bill "SPDXVersion: SPDX-2.3\n")
    foreach(entry IN ITEMS "Core|${lgpl}" "Widgets|${lgpl}" "Network|${lgpl}" "Charts|${gpl}"
                           "Lookalike|LGPL-3.0-only-like OR GPL-3.0-only"
                           "Closed|LicenseRef-Qt-Commercial")
        string(REPLACE "|" ";" entry "${entry}")
        list(GET entry 0 name)
        list(GET entry 1 license)
        string(APPEND bill "\nPackageName: ${name}\nPackageVersion: 0\n"
                           "PackageLicenseConcluded: ${license}\nFilesAnalyzed: false\n")
    endforeach()
    file(WRITE "${BLOOM_WORK}/qt/sbom/synthetic.spdx" "${bill}")
endfunction()

function(run result_variable output_variable)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DBLOOM_SOURCE_ROOT=${BLOOM_WORK}/source"
                "-DBLOOM_QT_MODULE_LIST=${BLOOM_WORK}/list.txt" -DBLOOM_QT_OPERATING_SYSTEM=linux
                ${ARGN} -P "${check}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    set(${result_variable} "${result}" PARENT_SCOPE)
    set(${output_variable} "${output}${error}" PARENT_SCOPE)
endfunction()

function(expect_failure what expected)
    run(result output ${ARGN})
    if(result EQUAL 0)
        message(FATAL_ERROR "${what}: the check passed")
    endif()
    if(NOT output MATCHES "${expected}")
        message(FATAL_ERROR "${what}: failed without the reason (${expected}):\n${output}")
    endif()
    message(STATUS "failed as expected: ${what}")
endfunction()

set(build "target_link_libraries(app PRIVATE Qt6::Core Qt6::Widgets)\n")
set(list "module Core all package\nmodule Widgets all package\nmodule Network linux package\n")
set(qt "-DBLOOM_QT_ROOT=${BLOOM_WORK}/qt")

write_tree("${build}" "${list}")
run(result output ${qt})
if(NOT result EQUAL 0 OR NOT output MATCHES "3 parts of linux offered under LGPL-3\\.0-only")
    message(FATAL_ERROR "a build that names only listed LGPL parts was refused:\n${output}")
endif()
run(result output)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the list alone was refused without a locked Qt:\n${output}")
endif()

# A GPL-only module added to the build: refused by the list, with or without a locked Qt.
write_tree("${build}target_link_libraries(app PRIVATE Qt6::Charts)\n" "${list}")
expect_failure("a GPL-only module added to the build"
               "names the Qt part Charts, which the Qt module list does not have" ${qt})
expect_failure("a GPL-only module added to the build, without a locked Qt"
               "names the Qt part Charts, which the Qt module list does not have")
write_tree("${build}find_package(Qt6 6.8 REQUIRED COMPONENTS Charts)\n" "${list}")
expect_failure("a GPL-only module requested as a component"
               "names the Qt part Charts, which the Qt module list does not have" ${qt})

# Adding it to the list too does not get it through: its license decides.
write_tree("${build}target_link_libraries(app PRIVATE Qt6::Charts)\n"
           "${list}module Charts all package\n")
expect_failure("a GPL-only module added to the build and the list"
               "Qt Charts is offered under 'LicenseRef-Qt-Commercial OR GPL-3\\.0-only' only"
               ${qt})
write_tree("${build}" "${list}plugin Closed all package\n")
expect_failure("a part offered under a commercial license only"
               "Qt Closed is offered under 'LicenseRef-Qt-Commercial' only" ${qt})
# A license whose name merely contains the text is not the LGPL.
write_tree("${build}" "${list}module Lookalike all package\n")
expect_failure("a license that only resembles the LGPL" "Qt Lookalike is offered under" ${qt})
write_tree("${build}" "${list}module Not-A-Name all package\n")
expect_failure("an unreadable list line" "unreadable line" ${qt})
write_tree("${build}" "${list}module Absent all package\n")
expect_failure("a listed part the locked Qt does not have"
               "bill of materials has no part Absent" ${qt})
# A part listed for another operating system is not looked up here.
write_tree("${build}" "${list}module Absent windows package\n")
run(result output ${qt})
if(NOT result EQUAL 0)
    message(FATAL_ERROR "a part of another operating system was looked up:\n${output}")
endif()
file(REMOVE_RECURSE "${BLOOM_WORK}")
message(STATUS "BloomQtModuleCheck.cmake: every case passed")
