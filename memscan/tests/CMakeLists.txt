add_executable(oxymp_memscan_tests
    pattern_tests.cpp
)

target_link_libraries(oxymp_memscan_tests PRIVATE
    oxymp::memscan
    oxymp::options
    Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(oxymp_memscan_tests)
