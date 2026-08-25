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
#
# Это умолчание, а не правило для всех. Серверная сторона живёт иначе: рядом с
# ней лежит libnode.dll, собранный с динамической библиотекой, и через границу
# этой DLL ходят стандартные строки. Почему двух видов не избежать и как это
# устроено — в cmake/dual_runtime.cmake.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Общие настройки компиляции. Подключается к каждой цели как oxymp::options,
# чтобы флаги задавались в одном месте, а не копировались по CMakeLists.
add_library(oxymp_options INTERFACE)
add_library(oxymp::options ALIAS oxymp_options)

target_compile_features(oxymp_options INTERFACE cxx_std_20)

# Версия — одной строкой и из одного места.
#
# Она уже есть в project(...) и оттуда попадает в сведения о файле. Коду её
# приходится объявлять отдельно: второй список версий — это место, где однажды
# окажется не то, что показывает проводник.
target_compile_definitions(oxymp_options INTERFACE OXYMP_VERSION="${PROJECT_VERSION}")

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

# Двойная сборка общих библиотек: клиенту нужна статическая библиотека времени
# выполнения, серверу — динамическая. Подключается раньше зависимостей: ENet
# собирается тем же способом.
include(cmake/dual_runtime.cmake)

include(cmake/dependencies.cmake)

# Node.js — движок игровых режимов. Необязателен: без него собирается всё, кроме
# скриптов, и голый сервер от этого не меняется.
include(cmake/node.cmake)

# Подпись Authenticode. Объявляется раньше целей, чтобы oxymp_sign(...) была
# известна к моменту, когда её вызывают; по умолчанию не делает ничего.
include(cmake/sign.cmake)

# Сведения о файле: версия, издатель, описание. Тоже раньше целей — по той же
# причине, что и подпись.
include(cmake/version_info.cmake)

# Меню. Собирается своим средством и в свой срок — см. cmake/menu.cmake.
include(cmake/menu.cmake)

add_subdirectory(config)
add_subdirectory(memscan)
add_subdirectory(gamesig)
add_subdirectory(shared)
add_subdirectory(net)
add_subdirectory(script)
add_subdirectory(script-js)
add_subdirectory(client-js)
add_subdirectory(client)
add_subdirectory(server)
add_subdirectory(botclient)

if(WIN32)
    add_subdirectory(webui)
    add_subdirectory(cefui)
    add_subdirectory(launcher-patcher)
    add_subdirectory(launcher)
endif()

add_subdirectory(tools/sigcheck)

# Подпись наших бинарников вызывается в CMakeLists самих целей — там же, где они
# объявлены: add_custom_command(TARGET ...) иначе цель не видит. Подписываются
# только свои файлы; библиотеки Chromium уже подписаны проектом CEF, а средства
# разработки — бота, sigcheck, тесты — не раздают никому.

if(OXYMP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(config/tests)
    add_subdirectory(memscan/tests)
    add_subdirectory(shared/tests)
    add_subdirectory(client/tests)
    add_subdirectory(server/tests)
    add_subdirectory(script/tests)
    add_subdirectory(script-js/tests)
    add_subdirectory(client-js/tests)
endif()

# Раскладка собранного по каталогам, которые отдают людям.
#
# Каталог сборки для этого не годится и не должен: в нём лежат тесты, sigcheck,
# бот, отладочные символы и кеш Chromium — всё то, что нужно нам и не нужно
# никому больше. Раздавать его целиком значит просить человека разобраться, что
# из восьми исполняемых файлов запускать.
#
# Двумя каталогами, а не одним: клиент и сервер ставят разные люди на разные
# машины, и общего у них нет ничего, кроме версии протокола.
#
# Делает работу отдельный сценарий: раскладка зависит от того, что собрано, а
# узнать это на настройке нельзя — в многоконфигурационной сборке конфигурация
# выбирается позже.
if(WIN32)
    add_custom_target(oxymp_dist ALL
        COMMAND "${CMAKE_COMMAND}"
                -DDIST_CONFIG=$<CONFIG>
                -DDIST_BIN=$<TARGET_FILE_DIR:oxymp_launcher>
                -DDIST_ROOT=${CMAKE_SOURCE_DIR}/release
                -DDIST_MENU_PAGE=${OXYMP_MENU_PAGE}
                -P "${CMAKE_CURRENT_LIST_DIR}/cmake/dist_script.cmake"
        DEPENDS oxymp_launcher oxymp_client oxymp_launcher_patcher oxymp_server oxymp_cef_runtime
                oxymp_menu
        COMMENT "Раскладка по каталогам client и server"
        VERBATIM
    )

    # Скриптовые машины — отдельной зависимостью: их может не быть вовсе, когда
    # проект собран без Node, и упоминать их в DEPENDS безусловно нельзя.
    #
    # Без этой строки раскладка успевала отработать раньше сборки машины и
    # раскладывала предыдущую. Поймать это тяжело: файл на месте, размер похож,
    # а слой внутри — от прошлой сборки, и ресурс жалуется на то, чего в нём
    # давно нет.
    if(TARGET oxymp_client_js)
        add_dependencies(oxymp_dist oxymp_client_js)
    endif()
endif()
