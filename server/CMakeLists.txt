add_executable(oxymp_server
    src/main.cpp
    src/player_registry.cpp
    src/server.cpp
)

set_target_properties(oxymp_server PROPERTIES OUTPUT_NAME oxymp-server)

target_link_libraries(oxymp_server PRIVATE
    oxymp::net
    oxymp::shared
    oxymp::options
    spdlog::spdlog
)
