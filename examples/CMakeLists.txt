function(carafe_add_example name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE carafe::carafe)
    # Sanitizer/coverage flags come with carafe::carafe; warnings do not.
    carafe_target_warnings(${name})
endfunction()

carafe_add_example(hello)
