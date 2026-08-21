# Клиентское ядро: соединение с сервером и состояние сессии.
#
# Отдельная библиотека, а не часть DLL, потому что этим же кодом пользуется
# консольный бот-клиент. Сеть таким образом отлаживается без запуска игры.
add_library(oxymp_clientcore STATIC
    src/connection.cpp
    src/interpolation.cpp
)
add_library(oxymp::clientcore ALIAS oxymp_clientcore)

target_include_directories(oxymp_clientcore PUBLIC include)

target_link_libraries(oxymp_clientcore
    PUBLIC oxymp::net oxymp::shared
    PRIVATE oxymp::options spdlog::spdlog
)

target_compile_features(oxymp_clientcore PUBLIC cxx_std_20)

include(embed_text)

# Модуль, который лаунчер внедряет в процесс игры.
#
# Живёт в каталоге проекта: в папку игры не копируется ничего.
if(WIN32)
    add_library(oxymp_client SHARED
        src/client_app.cpp
        src/dllmain.cpp
        src/discord_presence.cpp
        src/game_session.cpp
        src/menu.cpp
        src/resource_cache.cpp
        src/script_host.cpp
        src/server_history.cpp
        src/ui_feed.cpp
        src/game/appearance.cpp
        src/game/bink_sound.cpp
        src/game/controls.cpp
        src/game/code_patch.cpp
        src/game/crash_log.cpp
        src/game/custom_text.cpp
        src/game/discord_block.cpp
        src/game/engine_addresses.cpp
        src/game/environment.cpp
        src/game/data_files.cpp
        src/game/file_device.cpp
        src/game/file_system.cpp
        src/game/focus_pause.cpp
        src/game/frontend.cpp
        src/game/hook.cpp
        src/game/hud.cpp
        src/game/intro.cpp
        src/game/keyboard_layout.cpp
        src/game/landing_page.cpp
        src/game/loaded_image.cpp
        src/game/menu_input.cpp
        src/game/native_table.cpp
        src/game/net_session.cpp
        src/game/network_bail.cpp
        src/game/network_game.cpp
        src/game/objects.cpp
        src/game/nameplates.cpp
        src/game/online_map.cpp
        src/game/overlay_renderer.cpp
        src/game/present_hook.cpp
        src/game/raw_input.cpp
        src/game/ped_activity.cpp
        src/game/ped_animation.cpp
        src/game/ped_appearance.cpp
        src/game/player.cpp
        src/game/remote_players.cpp
        src/game/respawn.cpp
        src/game/screen.cpp
        src/game/session_state.cpp
        src/game/script_startup.cpp
        src/game/story.cpp
        src/game/streaming.cpp
        src/game/streaming_files.cpp
        src/game/script_tick.cpp
        src/game/text_entry.cpp
        src/game/ui_layer.cpp
        src/game/vehicle_snapshot.cpp
        src/game/vehicles.cpp
        src/game/window.cpp
        src/game/world.cpp
        src/game/world_hold.cpp
        oxymp-client.rc
    )

    set_target_properties(oxymp_client PROPERTIES
        OUTPUT_NAME oxymp-client
        PREFIX ""
    )

    # gamesig нужен только здесь: работа с внутренностями игры начинается и
    # заканчивается в модуле, который в неё внедряется. Бот-клиент, собранный
    # из того же ядра, про игру не знает вовсе.
    oxymp_embed_text(oxymp_client ui/overlay.html overlayPage oxymp::client::game::ui)

    # Страница меню — внутрь модуля, ресурсом.
    #
    # Прежде она лежала файлом рядом с клиентом, в каталоге ui, и это было
    # уступкой размеру: четыре с половиной мегабайта байтовым массивом
    # компилятор собирал бы минутами. Ресурс снимает эту цену целиком — его
    # содержимое просто складывается в файл, компилятору его не видно, — а
    # раздача перестаёт зависеть от каталога, который можно потерять или
    # подменить.
    if(EXISTS "${OXYMP_MENU_PAGE}")
        # Обратные косые в пути ресурса удваиваются: rc читает их как знак
        # escape, и один путь с C:\ превратился бы в путь с забоем.
        string(REPLACE "\\" "\\\\" OXYMP_MENU_PAGE_RC "${OXYMP_MENU_PAGE}")

        configure_file(menu_page.rc.in "${CMAKE_CURRENT_BINARY_DIR}/menu_page.rc" @ONLY)
        target_sources(oxymp_client PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/menu_page.rc")

        # Страницу собирает vite, и собрать её нужно раньше, чем ресурс возьмёт
        # её содержимое.
        add_dependencies(oxymp_client oxymp_menu)
    else()
        message(WARNING "нет собранного меню: интерфейс у игрока будет пустым")
    endif()

    # Заголовок границы со скриптовой машиной.
    #
    # Путём, а не целью, и это намеренно. Машина собирается только при наличии
    # Node, а клиент обязан собираться и без него: в этом случае он просто не
    # находит её на диске и говорит об этом один раз в журнал. Зависимость от
    # цели заставила бы оборачивать в условие и сам клиент.
    #
    # Ничего, кроме типов C, в заголовке нет — именно затем, чтобы его можно
    # было включить сюда, в сборку с /MT.
    target_include_directories(oxymp_client PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../client-js/include")

    # d3d11 и d3dcompiler — интерфейс рисуется прямо в кадр игры, а не окном
    # поверх неё. cefui — сама страница, отдающая готовые точки.
    target_link_libraries(oxymp_client PRIVATE
        minhook::minhook
        oxymp::clientcore
        oxymp::config
        oxymp::gamesig
        oxymp::options
        oxymp::cefui
        nlohmann_json::nlohmann_json
        spdlog::spdlog
        d3d11
        dxgi
        d3dcompiler

        # Версия исполняемого файла игры и перечень загруженных модулей —
        # см. src/game/environment.cpp.
        version
        psapi

        # Загрузка ресурсов с мини-сайта сервера. Библиотека отложенная — см.
        # список DELAYLOAD, — и подгружается только когда сервер что-то раздаёт.
        wininet
    )
endif()

if(WIN32)
    oxymp_version_info(oxymp_client "oxyMP — игровая часть клиента")
endif()

# Подпись внедряемого модуля. Пустышка, пока OXYMP_SIGN выключен.
oxymp_sign(oxymp_client)
