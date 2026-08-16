# Настройки клиента: то, что переживает запуск.
#
# Отдельным модулем, а не частью клиента или лаунчера, потому что читают их оба.
# Лаунчер записывает туда путь к игре, имя игрока и последний адрес; внедряемый
# модуль читает оттуда всё остальное и отдаёт странице меню. Общий файл, у
# которого два хозяина, обязан иметь одно описание — иначе они разойдутся в том,
# что в нём лежит.
#
# Не зависит ни от чего нашего. Это условие: модуль, знающий про сеть или про
# игру, нельзя было бы подключить к лаунчеру, который не знает ни того, ни
# другого.
add_library(oxymp_config STATIC
    src/settings.cpp
    src/skin.cpp
)
add_library(oxymp::config ALIAS oxymp_config)

target_include_directories(oxymp_config PUBLIC include)

target_link_libraries(oxymp_config
    PRIVATE oxymp::options tomlplusplus::tomlplusplus nlohmann_json::nlohmann_json
            spdlog::spdlog
)

# Разбор без исключений.
#
# toml++ по умолчанию бросает исключение на испорченном файле, а испорченный файл
# настроек — не исключительный случай, а обычный: его правят руками. С этим
# признаком неудача приходит возвращаемым значением, и обходиться с ней можно
# там же, где она случилась, — взять умолчания и написать об этом в журнал.
target_compile_definitions(oxymp_config PRIVATE TOML_EXCEPTIONS=0)

target_compile_features(oxymp_config PUBLIC cxx_std_20)
