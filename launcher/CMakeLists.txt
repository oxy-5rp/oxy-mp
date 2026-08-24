include(embed_text)

add_executable(oxymp_launcher
    src/game_choice.cpp
    src/game_locator.cpp
    src/game_mirror.cpp
    src/game_platform.cpp
    src/game_settings.cpp
    src/game_process.cpp
    src/game_store.cpp
    src/launcher_patch.cpp
    src/launcher_window.cpp
    src/main.cpp
    src/page_text.cpp
    src/paths.cpp
    src/process_inject.cpp
    src/process_lookup.cpp
    src/registry.cpp
    src/rockstar_launcher.cpp
    src/session.cpp
    src/setup_window.cpp
    src/skin_assets.cpp

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

# Страница окна лаунчера лежит рядом с кодом обычным html и встраивается в
# бинарник при сборке: рядом с исполняемым файлом ей делать нечего — потеряется
# или будет заменена. Оформление при этом приходит не отсюда, а из skin.bin:
# страница знает только, куда его подставить.
oxymp_embed_text(oxymp_launcher ui/launcher_page.html launcherPage oxymp::launcher::ui)

# Страница окна установки — того, в котором спрашивают каталог игры и площадку.
# Показывается один раз, пока в oxymp.toml нет пути к игре.
oxymp_embed_text(oxymp_launcher ui/setup_page.html setupPage oxymp::launcher::ui)

target_link_libraries(oxymp_launcher PRIVATE
    oxymp::shared
    oxymp::options
    oxymp::webui
    spdlog::spdlog

    # Оформление окна: имя, значок, фон и его размер лежат в skin.bin, и читает
    # его тот же модуль, которым его читает клиент. Два разбора одного файла
    # разошлись бы на первой же правке.
    oxymp::config

    # Ради одной величины — версии игры, под которую выверены сигнатуры.
    # Держать её вторым списком в лаунчере значило бы завести место, где она
    # разойдётся с настоящей.
    oxymp::gamesig
)

# Версия исполняемого файла игры. Ею лаунчер отличает подходящую сборку от
# чужой до запуска, а не после.
target_link_libraries(oxymp_launcher PRIVATE version)

# Шифр Windows. Ради одного дела: список установленных игр Rockstar Games
# Launcher лежит зашифрованным, и без него не найти копию, купленную в Steam или
# Epic, — в реестре они записаны под другими именами значений.
target_link_libraries(oxymp_launcher PRIVATE bcrypt)

# Окно выбора каталога в установке. IFileOpenDialog живёт в оболочке Windows, а
# заводится он через COM — отсюда обе библиотеки.
target_link_libraries(oxymp_launcher PRIVATE ole32 shell32)

# Лаунчер бесполезен без того, что он внедряет: модуля в игру, подмены звена
# BattlEye в лаунчер Rockstar и хозяйства Chromium, которым модуль рисует
# интерфейс изнутри игры.
add_dependencies(oxymp_launcher oxymp_client oxymp_rglpatch oxymp_cefsub oxymp_cef_runtime)

oxymp_version_info(oxymp_launcher "oxyMP — запуск игры и подключение к серверу")

# Подпись. Лаунчер — то, что запускает игрок, и первое, обо что спотыкается
# SmartScreen; ему подпись нужнее всего. Пустышка, пока OXYMP_SIGN выключен.
oxymp_sign(oxymp_launcher)
