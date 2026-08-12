# Встраивание текстовых файлов в бинарник.
#
# Нужно, чтобы страницы интерфейса не приходилось класть рядом с исполняемым
# файлом. Клиент раздаётся друзьям одним каталогом, и чем меньше в нём того, что
# можно потерять или перепутать, тем меньше поводов для разбирательств.

set(OXYMP_EMBED_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/embed_text_script.cmake")

# Порождает заголовок с содержимым файла и добавляет его к цели.
#
# Порождение привязано к сборке, а не к настройке: правка страницы попадает в
# следующую же сборку, без повторного запуска cmake.
function(oxymp_embed_text target input symbol name_space)
    get_filename_component(absolute "${input}" ABSOLUTE)
    get_filename_component(stem "${input}" NAME_WE)

    set(generated "${CMAKE_CURRENT_BINARY_DIR}/embedded/${stem}.hpp")

    add_custom_command(
        OUTPUT "${generated}"
        COMMAND "${CMAKE_COMMAND}"
                -DEMBED_INPUT=${absolute}
                -DEMBED_OUTPUT=${generated}
                -DEMBED_SYMBOL=${symbol}
                -DEMBED_NAMESPACE=${name_space}
                -P "${OXYMP_EMBED_SCRIPT}"
        DEPENDS "${absolute}" "${OXYMP_EMBED_SCRIPT}"
        COMMENT "Встраивание ${stem}"
        VERBATIM
    )

    target_sources(${target} PRIVATE "${generated}")
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/embedded")
endfunction()
