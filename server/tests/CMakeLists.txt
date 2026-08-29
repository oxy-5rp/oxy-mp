# Тесты тех частей сервера, у которых есть собственные правила, а не пересылка.
#
# Кому вести какую машину, что скрипту позволено сделать с сессией, как читается
# конфигурационный файл и как идут часы — всё это решается без сети и проверяется
# без неё же. Сокеты в этот список не попадают намеренно: там нечего решать.
add_executable(oxymp_server_tests
    attachment_directory_tests.cpp
    config_file_tests.cpp
    console_tests.cpp
    game_data_tests.cpp
    resource_catalog_tests.cpp
    server_core_tests.cpp
    state_bundler_tests.cpp
    vehicle_directory_tests.cpp
    world_clock_tests.cpp
    ../src/config_file.cpp
    ../src/console.cpp
    ../src/game_data.cpp
    ../src/object_directory.cpp
    ../src/ped_directory.cpp
    ../src/resource_catalog.cpp
    ../src/player_registry.cpp
    ../src/server_core.cpp
    ../src/vehicle_directory.cpp
    ../src/world_clock.cpp
)

target_include_directories(oxymp_server_tests PRIVATE ../src)

# Сеть — только ради заголовка с номером соединения: игрок в реестре назван и
# им тоже. Сокетов здесь не поднимается ни одного.
target_link_libraries(oxymp_server_tests PRIVATE
    oxymp::net
    oxymp::script
    oxymp::shared
    oxymp::options
    spdlog::spdlog
    Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(oxymp_server_tests)
