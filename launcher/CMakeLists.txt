include(embed_text)

add_executable(oxymp_launcher
    src/connect_window.cpp
    src/game_locator.cpp
    src/game_mirror.cpp
    src/game_settings.cpp
    src/game_process.cpp
    src/launcher_patch.cpp
    src/main.cpp
    src/paths.cpp
    src/process_inject.cpp
    src/process_lookup.cpp
    src/registry.cpp
    src/rockstar_launcher.cpp
    src/session.cpp

    # Значок: лаунчер — это то, что игрок видит в проводнике и на панели задач.
    oxymp.rc
)

set_target_properties(oxymp_launcher PROPERTIES OUTPUT_NAME oxymp)

# Оконное приложение, а не консольное.
#
# Консоль пряталась и раньше, но пряталась уже созданной: чёрное окно успевало
# мигнуть при каждом запуске. Windows создаёт её по разряду приложения, и не
# создать её можно только объявившись оконным.
#
# Точка входа остаётся обычной. Оконный разряд требует WinMain, но линкер
# принимает и main, если сказать ему это прямо, — а два входа ради одного
# свойства были бы двумя местами, где чинить одно и то же.
set_target_properties(oxymp_launcher PROPERTIES WIN32_EXECUTABLE TRUE)
target_link_options(oxymp_launcher PRIVATE /ENTRY:mainCRTStartup)

# Страница окна подключения лежит рядом с кодом обычным html и встраивается в
# бинарник при сборке: рядом с исполняемым файлом ей делать нечего — потеряется
# или будет заменена.
oxymp_embed_text(oxymp_launcher ui/connect.html connectPage oxymp::launcher::ui)

target_link_libraries(oxymp_launcher PRIVATE
    oxymp::shared
    oxymp::options
    oxymp::webui
    spdlog::spdlog

    # Ради одной величины — версии игры, под которую выверены сигнатуры.
    # Держать её вторым списком в лаунчере значило бы завести место, где она
    # разойдётся с настоящей.
    oxymp::gamesig
)

# Версия исполняемого файла игры. Ею лаунчер отличает подходящую сборку от
# чужой до запуска, а не после.
target_link_libraries(oxymp_launcher PRIVATE version)

# Лаунчер бесполезен без того, что он внедряет: модуля в игру, подмены звена
# BattlEye в лаунчер Rockstar и хозяйства Chromium, которым модуль рисует
# интерфейс изнутри игры.
add_dependencies(oxymp_launcher oxymp_client oxymp_rglpatch oxymp_cefsub oxymp_cef_runtime)
