# Сведения о файле: версия, издатель, описание.
#
# Зачем это нужно, кроме опрятности. Исполняемый файл без версии и описания —
# первый признак, по которому машинные проверки антивирусов считают файл
# подозрительным: у настоящих программ эти сведения есть всегда, у наспех
# собранного вредоноса — почти никогда. Само по себе это не лечит ничего (см.
# раздел про антивирусы в README), но и отдавать им лишний довод бесплатно
# незачем.
#
# Заводится одним местом на все цели: у пяти бинарников одна версия проекта и
# один издатель, и пять копий одного и того же разошлись бы через месяц.

set(OXYMP_VERSION_INFO_TEMPLATE "${CMAKE_CURRENT_LIST_DIR}/version_info.rc.in")

# Версия четырьмя числами через запятую — так её требует ресурс. Четвёртое
# число проекту не нужно, но обязано быть.
set(OXYMP_VERSION_FIELDS "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")

# Добавляет цели ресурс со сведениями о ней.
#
# description — то, что видит человек в свойствах файла и в диспетчере задач.
# По-русски, как и всё, что читают люди.
function(oxymp_version_info target description)
    if(NOT WIN32)
        return()
    endif()

    get_target_property(type ${target} TYPE)
    if(type STREQUAL "SHARED_LIBRARY")
        set(OXYMP_FILE_TYPE VFT_DLL)
        set(suffix ".dll")
    else()
        set(OXYMP_FILE_TYPE VFT_APP)
        set(suffix ".exe")
    endif()

    get_target_property(output ${target} OUTPUT_NAME)
    if(NOT output)
        set(output ${target})
    endif()

    set(OXYMP_FILE_DESCRIPTION "${description}")
    set(OXYMP_INTERNAL_NAME "${output}")
    set(OXYMP_ORIGINAL_FILENAME "${output}${suffix}")

    set(generated "${CMAKE_CURRENT_BINARY_DIR}/version_info_${target}.rc")
    configure_file("${OXYMP_VERSION_INFO_TEMPLATE}" "${generated}" @ONLY)

    # Кодовая страница UTF-8 задаётся явно: configure_file пишет UTF-8, а rc.exe
    # без указания читает файл в кодировке системы — и русское описание
    # превратилось бы в набор знаков вопроса прямо в свойствах файла.
    set_source_files_properties("${generated}" PROPERTIES COMPILE_OPTIONS "/c65001")

    target_sources(${target} PRIVATE "${generated}")
endfunction()
