# Проверки серверной скриптовой машины.
#
# Собираются, только когда собрана сама машина: без Node её нет вовсе.
#
# Набора здесь долго не было, и это стоило дорого: ресурс, отказавший при
# запуске, ронял сервер целиком — молча, с разбором стека V8. У клиентской машины
# такая проверка была, и та же поломка в ней не жила ни дня.
if(NOT TARGET oxymp_script_js)
    return()
endif()

# Без Catch2, и это не небрежность: машина собирается с динамической библиотекой
# времени выполнения (рядом с ней живёт libnode.dll), а Catch2 в этом проекте —
# со статической, потому что с ней собран клиент. Линковщик их не смешивает и
# прав: две кучи в одном процессе рано или поздно портят память. Дробность в
# ctest от этого не теряется — имя проверки уходит доводом.
add_executable(oxymp_script_js_tests
    engine_tests.cpp
)

target_link_libraries(oxymp_script_js_tests PRIVATE
    oxymp::script_js
    oxymp::options
)

set_target_properties(oxymp_script_js_tests PROPERTIES
    MSVC_RUNTIME_LIBRARY "${OXYMP_DYNAMIC_RUNTIME}")

# libnode.dll машина ищет сама, рядом с исполняемым файлом (см. node_library.cpp).
# Проверка ложится туда же, куда и сервер, и находит ту же копию — а значит
# проверяется заодно и раскладка, которая уезжает людям.
add_dependencies(oxymp_script_js_tests oxymp_server)

foreach(case
    broken-entry-is-refused
    absent-name-is-refused
    whole-resource-rises-after-broken-one
    missing-entry-is-named
    escaping-entry-is-refused)

    add_test(NAME "script-js: ${case}"
             COMMAND oxymp_script_js_tests "${case}")
endforeach()
