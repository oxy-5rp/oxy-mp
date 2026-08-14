add_executable(oxymp_server
    src/config_file.cpp
    src/http_server.cpp
    src/main.cpp
    src/player_registry.cpp
    src/resource_catalog.cpp
    src/resource_store.cpp
    src/server.cpp
    src/server_core.cpp
    src/object_directory.cpp
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

    # libnode.dll кладётся рядом с собранным сервером сразу же.
    #
    # Иначе запустить его из каталога сборки было бы нельзя: Windows ищет
    # библиотеки рядом с исполняемым файлом, а не там, откуда его собирали. Ждать
    # раскладки по каталогам раздачи ради того, чтобы проверить одну правку, —
    # лишний шаг на каждом обороте.
    add_custom_command(TARGET oxymp_server POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${OXYMP_NODE_DLL}" "$<TARGET_FILE_DIR:oxymp_server>"
        COMMENT "libnode.dll рядом с сервером"
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
# Здесь это решается тем, чем не решается у клиента: сервер — обычный
# исполняемый файл, и Windows ищет его зависимости в его же каталоге. Клиент
# такого не может — он живёт в чужом процессе, и ищут ему рядом с GTA5.exe.
#
# Только выпускные: отладочные Microsoft распространять запрещает, и сборка их
# сюда не берёт (CMAKE_INSTALL_DEBUG_LIBRARIES не выставляется).
if(MSVC)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    include(InstallRequiredSystemLibraries)

    foreach(runtime_library IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        add_custom_command(TARGET oxymp_server POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${runtime_library}" "$<TARGET_FILE_DIR:oxymp_server>"
            VERBATIM
        )
    endforeach()
endif()

# Подпись. Сервер антивирус не трогает — он ни во что не внедряется, — но
# подписать стоит и его: подписанный сервер вызывает больше доверия, а стоит это
# ровно ничего. Пустышка, пока OXYMP_SIGN выключен.
oxymp_sign(oxymp_server)
