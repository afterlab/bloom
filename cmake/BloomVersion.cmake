# The compact form of a version label, the same mapping as bloom::core::buildVersionOf
# (src/core/include/bloom/core/version.hpp), where CMake needs it at configure time (the macOS
# bundle's build version). The standard comparison of the systems that read it ranks
# 0.1.0-alpha.1 above 0.1.0 and ranks 0.1.0a1 below it.
#
#   bloom_build_version(<out> <label>)   0.1.0-alpha.1 -> 0.1.0a1, -beta.N -> bN, -rc.N -> rcN;
#                                        any other label is returned unchanged.

include_guard(GLOBAL)

function(bloom_build_version output label)
    if(label MATCHES "^([0-9]+\\.[0-9]+\\.[0-9]+)-(alpha|beta|rc)\\.([0-9]+)$")
        set(compact "${CMAKE_MATCH_2}")
        if(compact STREQUAL "alpha")
            set(compact "a")
        elseif(compact STREQUAL "beta")
            set(compact "b")
        endif()
        set(label "${CMAKE_MATCH_1}${compact}${CMAKE_MATCH_3}")
    endif()
    set(${output} "${label}" PARENT_SCOPE)
endfunction()
