add_executable(oxymp_botclient
    src/main.cpp
)

set_target_properties(oxymp_botclient PROPERTIES OUTPUT_NAME oxymp-botclient)

target_link_libraries(oxymp_botclient PRIVATE
    oxymp::clientcore
    oxymp::options
    spdlog::spdlog
)
