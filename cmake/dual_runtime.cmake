# Библиотеки, нужные обеим сторонам, собираются дважды.
#
# Причина — библиотека времени выполнения, и обе стороны выбирают её не от вкуса.
#
# **Клиент обязан быть статическим** (`/MT`). Он внедряется в чужой процесс, и
# зависимости ему Windows ищет рядом с `GTA5.exe`, а не рядом с ним самим:
# положенная в каталог клиента `msvcp140.dll` попросту не нашлась бы.
#
# **Сервер обязан быть динамическим** (`/MD`). Рядом с ним живёт `libnode.dll`,
# собранный так же — иначе Node не собирается вовсе, — и через границу этой
# библиотеки ходят стандартные строки и умные указатели. На двух разных кучах это
# не «немного неаккуратно», а порча памяти: выделено там, освобождается здесь.
#
# Смешать `/MT` и `/MD` в одном исполняемом файле нельзя, и линковщик говорит об
# этом прямо (LNK2038). Значит, общее нужно в двух видах.
#
# Цена этого решения — двойная сборка нескольких небольших библиотек. Она мала:
# все они вместе — десяток файлов.

set(OXYMP_STATIC_RUNTIME  "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(OXYMP_DYNAMIC_RUNTIME "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# Заводит статический и динамический вид одной библиотеки.
#
#   oxymp_library(net
#       SOURCES  src/host.cpp
#       INCLUDES include
#       PUBLIC   oxymp::shared
#       PRIVATE  enet::enet oxymp::options)
#
# Получаются цели `oxymp_net` и `oxymp_net_md` с псевдонимами `oxymp::net` и
# `oxymp::net_md`.
#
# Зависимости у динамического вида подменяются сами: если у названной цели есть
# двойник с суффиксом `_md`, берётся он. Так `oxymp::net_md` подхватывает
# `oxymp::shared_md`, ничего об этом не зная. Цели без двойника — например
# `oxymp::options`, у которой нет ни одного объектного файла, — берутся как есть.
function(oxymp_library name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "SOURCES;INCLUDES;PUBLIC;PRIVATE")

    foreach(kind IN ITEMS static dynamic)
        if(kind STREQUAL "static")
            set(target "oxymp_${name}")
            set(runtime "${OXYMP_STATIC_RUNTIME}")
            set(alias "oxymp::${name}")
        else()
            set(target "oxymp_${name}_md")
            set(runtime "${OXYMP_DYNAMIC_RUNTIME}")
            set(alias "oxymp::${name}_md")
        endif()

        add_library(${target} STATIC ${ARG_SOURCES})
        add_library(${alias} ALIAS ${target})

        set_target_properties(${target} PROPERTIES MSVC_RUNTIME_LIBRARY "${runtime}")

        foreach(directory IN LISTS ARG_INCLUDES)
            target_include_directories(${target} PUBLIC ${directory})
        endforeach()

        oxymp_link_matching(${target} PUBLIC "${kind}" "${ARG_PUBLIC}")
        oxymp_link_matching(${target} PRIVATE "${kind}" "${ARG_PRIVATE}")

        target_compile_features(${target} PUBLIC cxx_std_20)
    endforeach()
endfunction()

# Подключает зависимости, подменяя их двойниками там, где двойник есть.
function(oxymp_link_matching target visibility kind dependencies)
    foreach(dependency IN LISTS dependencies)
        if(kind STREQUAL "dynamic" AND TARGET "${dependency}_md")
            target_link_libraries(${target} ${visibility} "${dependency}_md")
        else()
            target_link_libraries(${target} ${visibility} "${dependency}")
        endif()
    endforeach()
endfunction()
