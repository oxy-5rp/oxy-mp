# Внешние зависимости.
#
# Ни один чужой исходник не лежит в репозитории: всё тянется FetchContent'ом
# по зафиксированному тегу. Обновление версии — правка одной строки здесь.

include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

# ENet — надёжная доставка поверх UDP.
#
# Его собственный CMakeLists объявляет совместимость с CMake 2.8, а CMake 4 такие
# проекты уже не настраивает. Поэтому исходники только выкачиваются (SOURCE_SUBDIR
# указывает на несуществующий каталог, и подключения их сборки не происходит),
# а цель описывается здесь. Заодно это избавляет от их опций и переменных.
FetchContent_Declare(enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG        v1.3.18
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  cmake-build-is-not-used
)
FetchContent_MakeAvailable(enet)

add_library(enet STATIC
    "${enet_SOURCE_DIR}/callbacks.c"
    "${enet_SOURCE_DIR}/compress.c"
    "${enet_SOURCE_DIR}/host.c"
    "${enet_SOURCE_DIR}/list.c"
    "${enet_SOURCE_DIR}/packet.c"
    "${enet_SOURCE_DIR}/peer.c"
    "${enet_SOURCE_DIR}/protocol.c"
    "${enet_SOURCE_DIR}/unix.c"
    "${enet_SOURCE_DIR}/win32.c"
)
add_library(enet::enet ALIAS enet)

# SYSTEM: чужие заголовки не должны сыпать предупреждениями в наши сборки.
target_include_directories(enet SYSTEM PUBLIC "${enet_SOURCE_DIR}/include")

if(WIN32)
    target_link_libraries(enet PUBLIC ws2_32 winmm)
endif()

# Предупреждения чужого кода мы всё равно не исправляем, а наши на их фоне теряются.
if(MSVC)
    target_compile_options(enet PRIVATE /w)
else()
    target_compile_options(enet PRIVATE -w)
endif()

# spdlog — журналирование. Нужно всем трём исполняемым частям: сервер пишет в
# консоль, клиент внутри игры — в файл, и делает это из своего потока.
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.14.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(spdlog)

# MinHook — перехват функций игры.
#
# Нужен только клиенту: чтобы наш код получал управление внутри чужого процесса,
# начало функции игры подменяется переходом на наш обработчик. Делать это руками
# нельзя — под x64 придётся разбирать инструкции в начале функции, чтобы
# перенести их в трамплин, и ошибка здесь даёт вылет без объяснений.
#
# Как и у ENet, собственный CMakeLists не используется: он объявляет древнюю
# совместимость. Исходники перечислены явно.
if(WIN32)
    FetchContent_Declare(minhook
        GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
        GIT_TAG        v1.3.3
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  cmake-build-is-not-used
    )
    FetchContent_MakeAvailable(minhook)

    add_library(minhook STATIC
        "${minhook_SOURCE_DIR}/src/buffer.c"
        "${minhook_SOURCE_DIR}/src/hook.c"
        "${minhook_SOURCE_DIR}/src/trampoline.c"
        "${minhook_SOURCE_DIR}/src/hde/hde64.c"
    )
    add_library(minhook::minhook ALIAS minhook)

    target_include_directories(minhook SYSTEM PUBLIC "${minhook_SOURCE_DIR}/include")

    if(MSVC)
        target_compile_options(minhook PRIVATE /w)
    endif()
endif()

# WebView2 — движок Edge, которым рисуется наш интерфейс.
#
# Свой интерфейс приходится строить не средствами игры, и это не прихоть.
# Замеряно на живой игре: скриптовый тик, единственная точка, откуда доступны
# нативы рисования, доходит до клиента через шесть секунд после запуска
# стартовых скриптов — когда заставка Rockstar, реклама GTA Online и страница
# выбора режима уже позади. Нарисовать поверх них нативами нельзя ничем.
#
# Движков интерфейса два, и у каждого своё место.
#
# WebView2 — там, где страница живёт в своём окне: окно подключения лаунчера и
# экран загрузки поверх игры. Он уже установлен в системе как часть Windows и
# ничего с собой не тянет.
#
# CEF — внутри игры. Единственный из двух, кто умеет рисовать не в окно, а в
# память: OnPaint отдаёт готовые точки, и мы кладём их в кадр игры текстурой.
# WebView2 так не умеет вовсе, и обходной путь — снимок закадрового окна
# средствами Windows — работал, но оставался обходным: лишнее окно, лишний
# снимок и прозрачность, добытая порогом яркости вместо настоящего четвёртого
# канала. Так же устроен интерфейс у RAGE MP и alt:V, и по той же причине.
#
# Пакет WebView2 распространяется только через NuGet и представляет собой
# обычный zip. Собственной сборки в нём нет: заголовки и библиотека-загрузчик
# описываются здесь целью, как у ENet и MinHook.
if(WIN32)
    FetchContent_Declare(webview2
        URL      https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.2903.40
        URL_HASH SHA256=ef128016dd1e51c59178c827ed5b8aa3322c57afa8675d930f8109505542ad74
    )
    FetchContent_MakeAvailable(webview2)

    add_library(webview2 INTERFACE)
    add_library(webview2::webview2 ALIAS webview2)

    target_include_directories(webview2 SYSTEM INTERFACE
        "${webview2_SOURCE_DIR}/build/native/include"
    )
    target_link_libraries(webview2 INTERFACE
        "${webview2_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib"
    )

    # CEF — тот самый Chromium, что стоит за интерфейсом FiveM, RAGE MP и alt:V.
    #
    # Берётся готовая сборка Spotify — та же, что берут они: собирать Chromium
    # самим значит завести у себя многочасовую сборку ради того, что раздаётся
    # готовым.
    #
    # Разновидность minimal: в ней нет отладочных двоичных файлов и примеров —
    # только то, что нужно для работы, и исходники обёртки, которые обязан
    # собрать сам потребитель. Обёртка потому и в исходниках, что разговаривает
    # с libcef.dll по языку C: собранная чужим компилятором, она не подошла бы
    # никому.
    #
    # Версия закреплена: CEF меняет свой прикладной язык от выпуска к выпуску, и
    # «свежая» здесь означает «однажды перестанет собираться сама по себе».
    FetchContent_Declare(cef
        URL      https://cef-builds.spotifycdn.com/cef_binary_144.0.32%2Bg5ce7d26%2Bchromium-144.0.7559.258_windows64_minimal.tar.bz2
        URL_HASH SHA1=75dd3287e44c7026f20f2d7d69ec3a4337b2656b
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(cef)

    set(OXYMP_CEF_ROOT "${cef_SOURCE_DIR}" CACHE INTERNAL "Каталог дистрибутива CEF")
endif()

if(OXYMP_BUILD_TESTS)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.7.1
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(Catch2)

    # Модуль Catch.cmake (catch_discover_tests) при подключении через FetchContent
    # сам в CMAKE_MODULE_PATH не попадает — в отличие от find_package.
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()
