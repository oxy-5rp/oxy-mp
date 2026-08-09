# Внешние зависимости.
#
# Ни один чужой исходник не лежит в репозитории: всё тянется FetchContent'ом
# по зафиксированному тегу. Обновление версии — правка одной строки здесь.

include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

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
