# CompilerWarnings.cmake
#
# carafe_target_warnings(<target>)
#
# Applies a strict-but-practical warning set to a target, PRIVATEly so that
# consumers of the library never inherit our warning flags.

function(carafe_target_warnings target)
    set(gcc_like_warnings
        -Wall                   # the usual suspects
        -Wextra                 # a few more of them
        -Wpedantic              # warn on non-standard C++
        -Wshadow                # a variable declaration shadows an outer one
        -Wnon-virtual-dtor      # polymorphic base class without virtual dtor
        -Woverloaded-virtual    # a derived method hides a virtual one
        -Wold-style-cast        # (int)x instead of static_cast<int>(x)
        -Wcast-align            # a cast that may cause misaligned access
        -Wunused                # anything unused
        -Wconversion            # implicit conversions that may lose data
        -Wsign-conversion       # implicit signed/unsigned conversions
        -Wnull-dereference      # a null pointer dereference is detected
        -Wdouble-promotion      # float implicitly promoted to double
        -Wformat=2              # printf-family format string problems
        -Wimplicit-fallthrough  # a switch case falls through without [[fallthrough]]
    )

    set(msvc_warnings
        /W4
        /permissive-            # standards conformance mode
    )

    if(CARAFE_WARNINGS_AS_ERRORS)
        list(APPEND gcc_like_warnings -Werror)
        list(APPEND msvc_warnings /WX)
    endif()

    target_compile_options(${target} PRIVATE
        "$<$<CXX_COMPILER_ID:MSVC>:${msvc_warnings}>"
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:${gcc_like_warnings}>"
    )
endfunction()
