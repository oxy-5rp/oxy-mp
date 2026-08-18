# Проверки клиентской машины.
#
# Собираются, только когда собрана сама машина: без Node её нет вовсе, и
# проверять было бы нечего.
if(NOT TARGET oxymp_client_js)
    return()
endif()

add_executable(oxymp_client_js_tests
    module_tests.cpp
)

target_link_libraries(oxymp_client_js_tests PRIVATE
    oxymp::options
    Catch2::Catch2WithMain
)

# Заголовок границы — единственное, что нужно от машины: она грузится по имени,
# как её будет грузить и клиент.
target_include_directories(oxymp_client_js_tests PRIVATE ../include)

# Путь к библиотеке — при сборке, а не поиском во время работы: проверка обязана
# брать ту машину, которую только что собрали, а не найденную где-то рядом.
#
# Берётся разложенная копия из modules/js, а не собранная в bin. Разница не
# формальная: машина ищет libnode.dll рядом с собой, и лежащая в bin не нашла бы
# её никогда. Заодно так проверяется та самая раскладка, которая уезжает людям.
target_compile_definitions(oxymp_client_js_tests PRIVATE
    OXYMP_CLIENT_JS_LIBRARY="$<TARGET_FILE_DIR:oxymp_client_js>/${OXYMP_NODE_SUBDIRECTORY}/$<TARGET_FILE_NAME:oxymp_client_js>")

# Машина грузится по имени, поэтому обычной зависимости по компоновке нет — её
# нужно объявить вручную, иначе проверка соберётся раньше того, что проверяет.
add_dependencies(oxymp_client_js_tests oxymp_client_js)

include(Catch)
catch_discover_tests(oxymp_client_js_tests)
