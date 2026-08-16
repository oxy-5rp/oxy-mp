# Сборка страницы меню.
#
# Меню — обычное приложение на Vue, и собирается оно своим средством (vite), а не
# компилятором C++. Сборка проекта дёргает его сама: править страницу и отдельно
# вспоминать про npm — верный способ однажды положить в раздачу вчерашнее меню.
#
# Ничего не ломается, когда Node в системе нет. Node нужен только тому, кто
# страницу правит; тот, кто собирает клиент, получает уже собранную —
# `client/menu/dist/index.html` лежит в репозитории именно ради этого.

# Node ищется по трём местам, и порядок не случаен: сначала то, что человек
# поставил себе сам, потом то, что собрал проект для скриптового движка. Своя
# сборка идёт последней — она заведомо есть, но заведомо же не та, которую
# человек имел в виду, ставя Node.
find_program(OXYMP_NODE_EXECUTABLE
    NAMES node
    HINTS "${OXYMP_NODE_ROOT}/out/Release" "${OXYMP_NODE_ROOT}/out/Debug"
)

find_program(OXYMP_NPM_EXECUTABLE NAMES npm npm.cmd)

set(OXYMP_MENU_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../client/menu")
set(OXYMP_MENU_PAGE "${OXYMP_MENU_SOURCE}/dist/index.html" CACHE INTERNAL "Собранная страница меню")

# Собирает меню, если есть чем.
#
# Цель всегда объявляется, даже когда собрать нечем: без неё зависимости от неё
# пришлось бы обкладывать проверками в каждом месте, а это ровно тот сорт
# условий, которые однажды разъезжаются.
add_custom_target(oxymp_menu)

if(NOT OXYMP_NPM_EXECUTABLE)
    if(EXISTS "${OXYMP_MENU_PAGE}")
        message(STATUS "npm не найден: меню берётся уже собранным")
    else()
        message(WARNING
            "npm не найден и собранного меню нет: интерфейс у игрока будет пустым. "
            "Поставьте Node.js либо возьмите client/menu/dist/index.html из репозитория.")
    endif()
    return()
endif()

# Список исходников считается на этапе настройки, а не сборки, и это осознанно:
# новый файл в menu/src появляется куда реже, чем правка существующего, а
# пересчёт на каждую сборку стоил бы обхода дерева при всяком запуске.
file(GLOB_RECURSE OXYMP_MENU_SOURCES CONFIGURE_DEPENDS
    "${OXYMP_MENU_SOURCE}/src/*"
    "${OXYMP_MENU_SOURCE}/index.html"
    "${OXYMP_MENU_SOURCE}/package.json"
    "${OXYMP_MENU_SOURCE}/vite.config.ts"
)

# Зависимости ставятся один раз и только когда их нет.
#
# `npm ci` вместо `npm install` не случайно: он ставит ровно то, что записано в
# package-lock.json, и не правит его. Сборка, молча меняющая свои же исходники,
# — это сборка, у которой однажды не сойдётся то, что собралось у другого.
if(NOT EXISTS "${OXYMP_MENU_SOURCE}/node_modules")
    message(STATUS "меню: установка зависимостей (это долго и только в первый раз)")

    execute_process(
        COMMAND "${OXYMP_NPM_EXECUTABLE}" ci --no-audit --no-fund
        WORKING_DIRECTORY "${OXYMP_MENU_SOURCE}"
        RESULT_VARIABLE oxymp_menu_install_result
    )

    if(NOT oxymp_menu_install_result EQUAL 0)
        message(WARNING "меню: зависимости не встали — страница не пересоберётся")
        return()
    endif()
endif()

add_custom_command(
    OUTPUT "${OXYMP_MENU_PAGE}"
    COMMAND "${OXYMP_NPM_EXECUTABLE}" run build
    WORKING_DIRECTORY "${OXYMP_MENU_SOURCE}"
    DEPENDS ${OXYMP_MENU_SOURCES}
    COMMENT "Сборка меню"
    VERBATIM
)

add_custom_target(oxymp_menu_build DEPENDS "${OXYMP_MENU_PAGE}")
add_dependencies(oxymp_menu oxymp_menu_build)
