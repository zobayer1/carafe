# Sanitizers.cmake
#
# carafe_target_sanitizers(<target>)
#
# Instruments a target with AddressSanitizer + UndefinedBehaviorSanitizer when
# CARAFE_ENABLE_SANITIZERS is ON.
#
# carafe_target_coverage(<target>)
#
# Adds gcov instrumentation when CARAFE_ENABLE_COVERAGE is ON.
#
# Both use PUBLIC, which matters: ASan in particular has to instrument every
# translation unit that ends up in one binary, so the flags have to reach the
# tests and examples too. PUBLIC gets them there automatically -- anything that
# links carafe::carafe inherits them. Call these on the library only; calling
# them again on a dependent target just passes the same flags to the compiler
# twice. Contrast carafe_target_warnings(), which is PRIVATE by design so that
# our warning set never leaks into someone else's build.

function(carafe_target_sanitizers target)
    if(NOT CARAFE_ENABLE_SANITIZERS)
        return()
    endif()

    if(MSVC)
        target_compile_options(${target} PUBLIC /fsanitize=address)
        return()
    endif()

    set(flags -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_compile_options(${target} PUBLIC ${flags})
    target_link_options(${target} PUBLIC -fsanitize=address,undefined)
endfunction()

function(carafe_target_coverage target)
    if(NOT CARAFE_ENABLE_COVERAGE)
        return()
    endif()

    if(MSVC)
        message(WARNING "CARAFE_ENABLE_COVERAGE is not supported with MSVC")
        return()
    endif()

    target_compile_options(${target} PUBLIC --coverage -O0 -g)
    target_link_options(${target} PUBLIC --coverage)
endfunction()
