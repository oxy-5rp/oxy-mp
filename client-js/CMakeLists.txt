# Скриптовая машина клиента.
#
# Отдельной DLL, а не частью клиента, и это не выбор, а необходимость. Клиент
# собирается со статической библиотекой времени выполнения (`/MT`): он
# внедряется в чужой процесс, и зависимости ему Windows ищет рядом с
# `GTA5.exe`. Машина обязана быть динамической (`/MD`): рядом с ней живёт
# `libnode.dll`, собранный так же. Смешать их в одном бинарнике нельзя —
# линковщик говорит об этом прямо (LNK2038). Подробности — cmake/dual_runtime.cmake.
#
# Так же устроен и alt:V: его клиентский js-module — отдельная библиотека ровно
# по этой причине.
#
# Собирается, только когда Node найден. Без него клиент собирается как прежде и
# работает как прежде — просто без клиентских скриптов.
if(NOT OXYMP_WITH_JS OR NOT WIN32)
    return()
endif()

add_library(oxymp_client_js SHARED
    src/bindings.cpp
    src/convert.cpp
    src/module.cpp
    src/natives.cpp
    src/node_library.cpp
    src/resource.cpp
)

add_library(oxymp::client_js ALIAS oxymp_client_js)

set_target_properties(oxymp_client_js PROPERTIES
    OUTPUT_NAME oxymp-client-js
    MSVC_RUNTIME_LIBRARY "${OXYMP_DYNAMIC_RUNTIME}")

# Заголовок границы открыт наружу: его включает и клиент, собранный /MT. Ничего,
# кроме типов C, в нём нет — именно затем, чтобы это было возможно.
target_include_directories(oxymp_client_js PUBLIC include)

target_link_libraries(oxymp_client_js PRIVATE
    oxymp::node
    oxymp::options
)

target_compile_features(oxymp_client_js PRIVATE cxx_std_20)

# Слой alt:V встраивается в бинарник.
#
# Перечисления и общая часть берутся у сервера **теми же самыми файлами**, а не
# копиями. Это не экономия: `alt-shared` потому и зовётся общей частью, что на
# обеих сторонах она обязана быть одна и та же. Заведи мы здесь копию — она
# разошлась бы с серверной в первую же правку, и ресурс, считающий расстояние на
# клиенте и на сервере, получал бы два разных ответа.
include(embed_text)

oxymp_embed_text(oxymp_client_js ../script-js/js/alt_enums.js
                 altEnums oxymp::client::js::embedded)
oxymp_embed_text(oxymp_client_js ../script-js/js/alt_shared.js
                 altShared oxymp::client::js::embedded)
oxymp_embed_text(oxymp_client_js js/alt_natives_table.js
                 altNativesTable oxymp::client::js::embedded)
oxymp_embed_text(oxymp_client_js js/alt_natives_absent.js
                 altNativesAbsent oxymp::client::js::embedded)
oxymp_embed_text(oxymp_client_js js/alt_natives.js
                 altNatives oxymp::client::js::embedded)
oxymp_embed_text(oxymp_client_js js/alt_client_entities.js
                 altClientEntities oxymp::client::js::embedded)
oxymp_embed_text(oxymp_client_js js/alt_client_objects.js
                 altClientObjects oxymp::client::js::embedded)
oxymp_embed_text(oxymp_client_js js/alt_client_extras.js
                 altClientExtras oxymp::client::js::embedded)
oxymp_embed_text(oxymp_client_js js/alt_client_locals.js
                 altClientLocals oxymp::client::js::embedded)
oxymp_embed_text(oxymp_client_js js/alt_client_net.js
                 altClientNet oxymp::client::js::embedded)
oxymp_embed_text(oxymp_client_js js/alt_client.js
                 altClient oxymp::client::js::embedded)
oxymp_embed_text(oxymp_client_js js/alt_client_bootstrap.js
                 altClientBootstrap oxymp::client::js::embedded)

# Заголовки Node пишут в наши сборки предупреждениями, которых мы не исправим.
if(MSVC)
    target_compile_options(oxymp_client_js PRIVATE
        /wd4100  # неиспользованный довод: их полно в обработчиках V8
    )
endif()

# Машина кладётся туда же, где клиент её и ищет, — рядом с libnode.dll, которая
# ей нужна. Не в корень каталога клиента: там живёт то, что запускают и правят, а
# это хозяйство, которое трогать не надо. То же правило, что и у сервера. Каталог тот же, что у сервера (OXYMP_NODE_SUBDIRECTORY): движок один и
# тот же файл, и держать его в двух местах значило бы возить сто сорок мегабайт
# дважды.
add_custom_command(TARGET oxymp_client_js POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:oxymp_client_js>"
            "$<TARGET_FILE_DIR:oxymp_client_js>/${OXYMP_NODE_SUBDIRECTORY}/$<TARGET_FILE_NAME:oxymp_client_js>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${OXYMP_NODE_DLL}"
            "$<TARGET_FILE_DIR:oxymp_client_js>/${OXYMP_NODE_SUBDIRECTORY}/libnode.dll"
    COMMENT "Клиентская машина JS и libnode.dll в ${OXYMP_NODE_SUBDIRECTORY}"
    VERBATIM
)
