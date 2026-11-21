include_guard(GLOBAL)

# One test environment entry names one variable (#763).
#
# bloom_add_test reads its arguments with cmake_parse_arguments(PARSE_ARGV), which keeps a quoted
# argument whole: ENVIRONMENT "A=1;B=2" becomes ONE entry, "A=1;B=2", with its semicolons escaped,
# and CTest then sets a single variable A to "1;B=2" and never sets B. (A scale variant registered
# that way ran with QT_QPA_PLATFORM="offscreen;QT_SCALE_FACTOR=1.5", which Qt reads as a list of
# platforms to try, so it ran offscreen at scale 1 and nothing failed.) Each variable is passed as
# its own argument instead: ENVIRONMENT "A=1" "B=2".
#
# A value may still hold a semicolon (a search path is a list), so an entry is "joined" only when
# a semicolon in it is followed by another NAME=.
#
# bloom_joined_test_environment(<out> <list-variable>) sets <out> to the entries of the list held
# in <list-variable> that name more than one variable, as a list of their names joined with ' + '
# (for example "QT_QPA_PLATFORM + QT_SCALE_FACTOR"), or to an empty list.
function(bloom_joined_test_environment out list_variable)
    set(joined)
    foreach(entry IN LISTS ${list_variable})
        if(NOT entry MATCHES ";[A-Za-z_][A-Za-z0-9_]*=")
            continue()
        endif()
        # The names of every variable folded into this one entry, for the message.
        set(parts "${entry}")
        set(spelled)
        foreach(part IN LISTS parts)
            if(part MATCHES "^([A-Za-z_][A-Za-z0-9_]*)=")
                list(APPEND spelled "${CMAKE_MATCH_1}")
            endif()
        endforeach()
        list(JOIN spelled " + " spelled)
        list(APPEND joined "${spelled}")
    endforeach()
    set(${out} "${joined}" PARENT_SCOPE)
endfunction()
