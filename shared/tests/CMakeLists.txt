add_executable(oxymp_shared_tests
    message_tests.cpp
    mvalue_tests.cpp
    serialization_tests.cpp
)

target_link_libraries(oxymp_shared_tests PRIVATE
    oxymp::shared
    oxymp::options
    Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(oxymp_shared_tests)
