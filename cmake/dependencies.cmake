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
