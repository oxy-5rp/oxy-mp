add_executable(oxymp_server
    src/config_file.cpp
    src/console.cpp
    src/http_server.cpp
    src/main.cpp
    src/player_registry.cpp
    src/resource_catalog.cpp
    src/resource_store.cpp
    src/server.cpp
    src/server_core.cpp
    src/object_directory.cpp
    src/ped_directory.cpp
    src/vehicle_directory.cpp
    src/world_clock.cpp
)

# Значок. Только под Windows: под остальными системами ресурсов у файла нет, а
# сервер собирается и там.
if(WIN32)
    target_sources(oxymp_server PRIVATE oxymp-server.rc)
endif()

set_target_properties(oxymp_server PROPERTIES OUTPUT_NAME oxymp-server)

# Динамическая библиотека времени выполнения, в отличие от клиента.
#
# Рядом с сервером лежит libnode.dll, собранный так же — иначе Node не
# собирается вовсе, — и через границу этой библиотеки ходят стандартные строки и
# умные указатели. На двух разных кучах это порча памяти. Подробности и цена — в
# cmake/dual_runtime.cmake.
set_target_properties(oxymp_server PROPERTIES
    MSVC_RUNTIME_LIBRARY "${OXYMP_DYNAMIC_RUNTIME}")

# Отсюда и двойники библиотек: _md — те же исходники, собранные с динамической
# библиотекой времени выполнения.
#
# spdlog берётся заголовочным: у него нет двойника, а собранный вид пришлось бы
# заводить третьим. Заголовочный собирается вместе с потребителем и потому
# подходит любому.
target_link_libraries(oxymp_server PRIVATE
    oxymp::net_md
    oxymp::script_md
    oxymp::shared_md
    oxymp::options
    spdlog::spdlog_header_only
)

# Скриптовая машина подключается, только если Node найден. Без неё сервер
# собирается и работает как прежде — просто без скриптов, и говорит об этом
# прямо тому, кто перечислил ресурс на JS.
if(OXYMP_WITH_JS)
    target_sources(oxymp_server PRIVATE src/js_runtime.cpp)
    target_link_libraries(oxymp_server PRIVATE oxymp::script_js)
    target_compile_definitions(oxymp_server PRIVATE OXYMP_WITH_JS)

    # libnode.dll кладётся к собранному серверу сразу же — и в тот же каталог,
    # в который его кладёт раскладка раздачи.
    #
    # Иначе запустить сервер из каталога сборки было бы нельзя, а ждать
    # раскладки ради проверки одной правки — лишний шаг на каждом обороте.
    # Каталог обязан совпадать с тем, где движок ищет себя сам
    # (script-js/src/node_library.cpp): разъедутся — и сервер, собранный и
    # разложенный, не найдёт движка.
    add_custom_command(TARGET oxymp_server POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${OXYMP_NODE_DLL}"
                "$<TARGET_FILE_DIR:oxymp_server>/${OXYMP_NODE_SUBDIRECTORY}/libnode.dll"
        COMMENT "libnode.dll в ${OXYMP_NODE_SUBDIRECTORY}"
        VERBATIM
    )
endif()

# Сокеты раздачи. ENet тянет ту же библиотеку, но полагаться на чужую сборку
# нельзя: он вправе перестать это делать, и тогда ломаться будет здесь.
if(WIN32)
    target_link_libraries(oxymp_server PRIVATE ws2_32)
endif()

# Библиотеки Visual C++ ложатся рядом с сервером.
#
# Понадобились они оттого, что сервер собирается с динамической библиотекой
# времени выполнения, — и это ровно тот случай, ради которого весь остальной
# проект собирается со статической: на чужой машине их может не быть, а
# сообщение «не найден MSVCP140.dll» ничего не объясняет тому, кто его увидел.
#
# **Именно рядом, а не в подкаталоге, и это не небрежность.** Зависимости
# исполняемого файла Windows разрешает до первой его строки и ищет их только
# рядом с ним самим: подгрузить их по требованию, как libnode.dll, нельзя —
# на них стоит сам код, который эту подгрузку выполнял бы. Унести их в папку
# можно было бы одним способом: собрать сервер со статической библиотекой
# времени выполнения, а движок скриптов вынести отдельной DLL с границей на
# чистом C. Это большая работа, и её цена — три места вместо одного при всяком
# пополнении списка того, что доступно скрипту.
#
# Три и ровно три. Раньше сюда копировалось всё, что нашла CMake, — девять
# файлов, включая concrt140 и четыре спутника msvcp140. Ни один из них не нужен:
# ни сервер, ни libnode.dll на них не ссылаются (проверено `dumpbin /dependents`
# на обоих). Остальное, что им требуется, — api-ms-win-crt-* — часть самой
# Windows начиная с десятой и лежит в System32.
#
# Только выпускные: отладочные Microsoft распространять запрещает, и сборка их
# сюда не берёт (CMAKE_INSTALL_DEBUG_LIBRARIES не выставляется).
if(MSVC)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    include(InstallRequiredSystemLibraries)

    set(OXYMP_SERVER_RUNTIME_LIBRARIES
        msvcp140.dll
        vcruntime140.dll
        vcruntime140_1.dll
        CACHE INTERNAL "Библиотеки Visual C++, без которых сервер не запустится")

    foreach(runtime_library IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        get_filename_component(runtime_name "${runtime_library}" NAME)
        string(TOLOWER "${runtime_name}" runtime_name)

        if(NOT runtime_name IN_LIST OXYMP_SERVER_RUNTIME_LIBRARIES)
            continue()
        endif()

        add_custom_command(TARGET oxymp_server POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${runtime_library}" "$<TARGET_FILE_DIR:oxymp_server>"
            VERBATIM
        )
    endforeach()
endif()

if(WIN32)
    oxymp_version_info(oxymp_server "oxyMP — сервер сессии")
endif()

# Подпись. Сервер антивирус не трогает — он ни во что не внедряется, — но
# подписать стоит и его: подписанный сервер вызывает больше доверия, а стоит это
# ровно ничего. Пустышка, пока OXYMP_SIGN выключен.
oxymp_sign(oxymp_server)
