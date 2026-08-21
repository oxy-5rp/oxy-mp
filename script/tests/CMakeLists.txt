# Тесты скриптового слоя.
#
# Ядро здесь подставное — обычный список в памяти. Ради этого слой и объявлен
# интерфейсом: проверить, что ссылка на вышедшего игрока честно отвечает «его
# больше нет», можно не поднимая ни сети, ни игры.
add_executable(oxymp_script_tests
    dimension_tests.cpp
    entity_tests.cpp
    events_tests.cpp
)

target_link_libraries(oxymp_script_tests PRIVATE
    oxymp::script
    oxymp::options
    Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(oxymp_script_tests)
