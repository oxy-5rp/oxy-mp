add_executable(oxymp_server
    src/main.cpp
    src/player_registry.cpp
    src/server.cpp
)

# Значок. Только под Windows: под остальными системами ресурсов у файла нет, а
# сервер собирается и там.
if(WIN32)
    target_sources(oxymp_server PRIVATE oxymp-server.rc)
endif()

set_target_properties(oxymp_server PROPERTIES OUTPUT_NAME oxymp-server)

target_link_libraries(oxymp_server PRIVATE
    oxymp::net
    oxymp::shared
    oxymp::options
    spdlog::spdlog
)
