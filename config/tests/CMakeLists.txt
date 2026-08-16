add_executable(oxymp_config_tests
    settings_tests.cpp
    skin_tests.cpp
)

target_link_libraries(oxymp_config_tests PRIVATE
    oxymp::config
    oxymp::options
    Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(oxymp_config_tests)
