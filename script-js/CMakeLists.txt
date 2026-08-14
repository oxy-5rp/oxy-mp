# Скриптовая машина на Node.js.
#
# Отдельным модулем, а не частью сервера, и причина не в стройности. Заголовки
# Node тянут за собой V8, libuv и половину OpenSSL; окажись они в сервере —
# всякая правка в нём стоила бы пересборки этого хозяйства, а оно не быстрое.
# Здесь же граница узкая: наружу торчит один заголовок с тремя действиями.
#
# Собирается, только когда Node найден. Без него сервер собирается как прежде и
# работает как прежде — просто без скриптов.
if(NOT OXYMP_WITH_JS)
    return()
endif()

add_library(oxymp_script_js STATIC
    src/bindings.cpp
    src/convert.cpp
    src/engine.cpp
    src/resource.cpp
)

add_library(oxymp::script_js ALIAS oxymp_script_js)

target_include_directories(oxymp_script_js PUBLIC include)

# Динамическая библиотека времени выполнения — та же, что у libnode.dll и у
# сервера. Иначе стандартные строки, ходящие через границу Node, оказались бы на
# двух разных кучах.
set_target_properties(oxymp_script_js PROPERTIES
    MSVC_RUNTIME_LIBRARY "${OXYMP_DYNAMIC_RUNTIME}")

target_link_libraries(oxymp_script_js
    PUBLIC
        oxymp::script_md
    PRIVATE
        oxymp::node
        oxymp::options
        spdlog::spdlog_header_only
)

# Заголовки Node пишут в наши сборки предупреждениями, которых мы не исправим:
# собираются они своими флагами, не нашими, и в них полно того, что /W4 считает
# небрежностью. Подключены они как SYSTEM (см. cmake/node.cmake), но часть
# предупреждений MSVC этим не гасит.
if(MSVC)
    target_compile_options(oxymp_script_js PRIVATE
        /wd4100  # неиспользованный довод: их полно в обработчиках V8
    )
endif()
