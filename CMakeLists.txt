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

include(cmake/dependencies.cmake)

add_subdirectory(memscan)
add_subdirectory(gamesig)
add_subdirectory(shared)
add_subdirectory(net)
add_subdirectory(client)
add_subdirectory(server)
add_subdirectory(botclient)
add_subdirectory(tools/sigcheck)

if(OXYMP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(memscan/tests)
    add_subdirectory(shared/tests)
endif()
