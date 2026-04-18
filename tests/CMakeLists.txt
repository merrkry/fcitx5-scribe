find_package(Catch2 3 REQUIRED)

add_executable(
    scribe-tests
        engine.cpp
        config.cpp
)

target_link_libraries(
    scribe-tests
    PRIVATE
        Catch2::Catch2WithMain
        scribe_core
)

include(Catch)
catch_discover_tests(scribe-tests)
