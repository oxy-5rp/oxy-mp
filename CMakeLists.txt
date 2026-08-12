cmake_minimum_required(VERSION 3.28)

project(oxyMP
    VERSION 0.1.0
    DESCRIPTION "Минимальный мультиплеер для GTA V"
    LANGUAGES CXX C
)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "oxyMP собирается только под x64: игра 64-битная.")
endif()

option(OXYMP_BUILD_TESTS "Собирать юнит-тесты" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Стандартная библиотека вшивается в исполняемые файлы, а не подключается извне.
#
# Причина не в размере и не во вкусе: oxyMP раздаётся людям, и на чужой машине
# библиотек Visual C++ может не быть вовсе. Подключённая извне, она превращает
# запуск в «не найден MSVCP140.dll» — сообщение, из которого игрок не поймёт
# ничего и починить не сможет.
#
# С отладочной сборкой всё ещё хуже: она требует MSVCP140D.dll, VCRUNTIME140D.dll
# и ucrtbased.dll — а эти не только не стоят на чужой машине, но и не имеют права
# там оказаться: Microsoft запрещает их распространение. Собранный отладочно
# клиент неизбежно не запустится ни у кого, кроме того, кто его собрал.
#
# Вшитая библиотека снимает оба случая разом и не требует от игрока ничего.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Общие настройки компиляции. Подключается к каждой цели как oxymp::options,
# чтобы флаги задавались в одном месте, а не копировались по CMakeLists.
add_library(oxymp_options INTERFACE)
add_library(oxymp::options ALIAS oxymp_options)

target_compile_features(oxymp_options INTERFACE cxx_std_20)

if(MSVC)
    target_compile_options(oxymp_options INTERFACE
        /W4             # предупреждения: всё осмысленное
        /permissive-    # строгое соответствие стандарту
        /utf-8          # исходники и литералы в UTF-8
        /Zc:__cplusplus # честное значение __cplusplus
        /Zc:preprocessor
        /EHsc
    )
    target_compile_definitions(oxymp_options INTERFACE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

include(cmake/dependencies.cmake)

add_subdirectory(memscan)
add_subdirectory(gamesig)
add_subdirectory(shared)
add_subdirectory(net)
add_subdirectory(client)
add_subdirectory(server)
add_subdirectory(botclient)

if(WIN32)
    add_subdirectory(webui)
    add_subdirectory(cefui)
    add_subdirectory(rglpatch)
    add_subdirectory(launcher)
endif()

add_subdirectory(tools/sigcheck)

if(OXYMP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(memscan/tests)
    add_subdirectory(shared/tests)
endif()
