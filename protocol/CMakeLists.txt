add_library(wireview_protocol STATIC
    src/input.cpp
    src/protocol.cpp
)

target_include_directories(wireview_protocol
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(wireview_protocol PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(wireview_protocol PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(wireview_protocol PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(BUILD_TESTING)
    add_executable(wireview_protocol_tests
        tests/protocol_tests.cpp
    )
    target_link_libraries(wireview_protocol_tests PRIVATE wireview_protocol)

    if(MSVC)
        target_compile_options(wireview_protocol_tests PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(wireview_protocol_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()

    add_test(NAME wireview_protocol_tests COMMAND wireview_protocol_tests)
endif()
