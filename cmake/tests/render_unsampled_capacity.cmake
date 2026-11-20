# #751: a headless render whose capacity cannot be sampled completes. The test reading's unsampled
# form (BLOOM_TEST_FIXED_MEMORY_READING, src/core/include/bloom/core/test_fixed_memory_reading.hpp)
# makes the host sample unable to read the process memory limits, as on a Linux host without /sys,
# and makes a declared card's live budget query go unanswered. Admission must run on its stated
# assumption instead of waiting for a sample that never comes, and the render must say what it
# assumed.
#
#   cmake -DCLI=<bloom-cli> -DREADING=<physical>,<available>,<device> -DWORK_DIR=<dir>
#         -P render_unsampled_capacity.cmake
foreach(required CLI READING WORK_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "render_unsampled_capacity.cmake needs -D${required}=...")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(ENV{BLOOM_TEST_FIXED_MEMORY_READING} "${READING},unsampled")
execute_process(
    COMMAND "${CLI}" render --frame 0 --preset PngRgba8SrgbV1 --out "${WORK_DIR}/frame"
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
set(frame "${WORK_DIR}/frame/frame.png")
set(frame_start "")
if(EXISTS "${frame}")
    file(READ "${frame}" frame_start LIMIT 8 HEX)
endif()
file(REMOVE_RECURSE "${WORK_DIR}")

set(failed FALSE)
if(NOT result EQUAL 0 OR NOT frame_start STREQUAL "89504e470d0a1a0a")
    message(SEND_ERROR "the render with unsampled capacity did not complete (${result})")
    set(failed TRUE)
endif()
if(NOT output MATCHES "rendered 1 frame\\(s\\)")
    message(SEND_ERROR "the render did not report its published frame")
    set(failed TRUE)
endif()
# The host fallback is stated with what was assumed: half of the declared physical memory.
string(REGEX MATCH "^([0-9]+)," physical_field "${READING}")
math(EXPR half "${CMAKE_MATCH_1} / 2")
string(CONCAT host_line "warning[bloom-cli.render.capacity-assumed]: host capacity not sampled "
              "(process memory limit); assumed ${half} bytes (half of physical memory)")
string(FIND "${error}" "${host_line}" host_at)
if(host_at EQUAL -1)
    message(SEND_ERROR "the render did not say what it assumed for the unsampled host capacity")
    set(failed TRUE)
endif()
# A frame the device evaluated ran on the assumed device capacity, and says so.
if(output MATCHES "device gpu-frames=([0-9]+)" AND NOT CMAKE_MATCH_1 EQUAL 0)
    string(FIND "${error}" "device capacity not sampled (live device budget); assumed " device_at)
    if(device_at EQUAL -1)
        message(SEND_ERROR "a device frame did not say what it assumed for the unanswered budget")
        set(failed TRUE)
    endif()
endif()
if(failed)
    message(FATAL_ERROR "bloom-cli render output:\n${output}\n${error}")
endif()
message(STATUS "a render with unsampled capacity completed and said what it assumed")
