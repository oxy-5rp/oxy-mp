add_executable(oxymp_launcher
    src/game_locator.cpp
    src/game_process.cpp
    src/main.cpp
)

set_target_properties(oxymp_launcher PROPERTIES OUTPUT_NAME oxymp)

target_link_libraries(oxymp_launcher PRIVATE
    oxymp::shared
    oxymp::options
    spdlog::spdlog
)

# Лаунчер бесполезен без модуля, который он внедряет.
add_dependencies(oxymp_launcher oxymp_client)
