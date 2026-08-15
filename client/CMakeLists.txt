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
        src/resource_cache.cpp
        src/ui_feed.cpp
        src/game/appearance.cpp
        src/game/controls.cpp
        src/game/crash_log.cpp
        src/game/engine_addresses.cpp
        src/game/frontend.cpp
        src/game/hook.cpp
        src/game/hud.cpp
        src/game/intro.cpp
        src/game/landing_page.cpp
        src/game/loaded_image.cpp
        src/game/native_table.cpp
        src/game/net_session.cpp
        src/game/network_bail.cpp
        src/game/network_game.cpp
        src/game/objects.cpp
        src/game/nameplates.cpp
        src/game/online_map.cpp
        src/game/overlay_renderer.cpp
        src/game/page_source.cpp
        src/game/present_hook.cpp
        src/game/ped_activity.cpp
        src/game/ped_animation.cpp
        src/game/player.cpp
        src/game/remote_players.cpp
        src/game/respawn.cpp
        src/game/screen.cpp
        src/game/session_state.cpp
        src/game/script_startup.cpp
        src/game/story.cpp
        src/game/streaming.cpp
        src/game/script_tick.cpp
        src/game/text_entry.cpp
        src/game/ui_layer.cpp
        src/game/vehicle_snapshot.cpp
        src/game/vehicles.cpp
        src/game/window.cpp
        src/game/world.cpp
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

    # Картинка экрана загрузки встраивается тем же способом, что и разметка, и по
    # той же причине: страница доходит до CEF ссылкой с данными, у которой нет
    # ни файла на диске, ни соседей рядом с ним. Класть её рядом с клиентом
    # значило бы завести файл, который можно потерять, — а без него экран
    # загрузки остался бы пустым.
    oxymp_embed_text(oxymp_client ui/loading_art.jpg loadingArt oxymp::client::game::ui)

    # d3d11 и d3dcompiler — интерфейс рисуется прямо в кадр игры, а не окном
    # поверх неё. cefui — сама страница, отдающая готовые точки.
    target_link_libraries(oxymp_client PRIVATE
        minhook::minhook
        oxymp::clientcore
        oxymp::gamesig
        oxymp::options
        oxymp::cefui
        spdlog::spdlog
        d3d11
        dxgi
        d3dcompiler

        # Загрузка ресурсов с мини-сайта сервера. Библиотека отложенная — см.
        # список DELAYLOAD, — и подгружается только когда сервер что-то раздаёт.
        wininet
    )
endif()

# Подпись внедряемого модуля. Пустышка, пока OXYMP_SIGN выключен.
oxymp_sign(oxymp_client)
