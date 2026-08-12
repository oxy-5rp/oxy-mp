# Раскладывает собранное по двум каталогам: клиентский и серверный.
#
# Запускается сборкой, а не человеком. Ждёт от неё:
#   DIST_CONFIG — Release или Debug
#   DIST_BIN    — откуда брать собранное
#   DIST_ROOT   — куда раскладывать

# Раскладка только для release, и это не экономия времени.
#
# Отладочная сборка требует библиотек Visual C++, которые нельзя распространять,
# и на чужой машине не запустится в принципе. Каталог с ней выглядел бы готовым к
# раздаче, не будучи им, — а это худший вид неготовности.
if(NOT DIST_CONFIG STREQUAL "Release")
    return()
endif()

set(client "${DIST_ROOT}/client")
set(server "${DIST_ROOT}/server")

file(MAKE_DIRECTORY "${client}")
file(MAKE_DIRECTORY "${server}")

# Что нужно игроку, чтобы зайти в игру, и ничего сверх этого.
#
# Ни sigcheck, ни бота, ни тестов здесь нет намеренно: это средства разработки, и
# в каталоге, который получает человек, им делать нечего. Он не должен гадать,
# какой из восьми файлов запускать.
set(client_files
    oxymp.exe           # лаунчер, его и запускают
    oxymp-client.dll    # то, что внедряется в игру
    oxymp-rglpatch.dll  # подмена звена BattlEye в лаунчере Rockstar
)

set(server_files
    oxymp-server.exe
)

foreach(name IN LISTS client_files)
    if(EXISTS "${DIST_BIN}/${name}")
        file(COPY "${DIST_BIN}/${name}" DESTINATION "${client}")
    else()
        message(WARNING "нет ${name}: клиентский каталог будет неполным")
    endif()
endforeach()

foreach(name IN LISTS server_files)
    if(EXISTS "${DIST_BIN}/${name}")
        file(COPY "${DIST_BIN}/${name}" DESTINATION "${server}")
    else()
        message(WARNING "нет ${name}: серверный каталог будет неполным")
    endif()
endforeach()

# Chromium переносится целиком, кроме своего кеша.
#
# Кеш — это то, что браузер пишет себе сам во время работы: он появляется от
# запусков на этой машине, весит больше всего остального вместе взятого и на
# чужой машине не значит ровным счётом ничего. Попав в раздачу, он раздулся бы в
# каталоге игрока мёртвым грузом.
if(EXISTS "${DIST_BIN}/cef")
    file(GLOB_RECURSE cef_files RELATIVE "${DIST_BIN}/cef" "${DIST_BIN}/cef/*")

    foreach(relative IN LISTS cef_files)
        if(relative MATCHES "^cache/")
            continue()
        endif()

        get_filename_component(subdirectory "${relative}" DIRECTORY)

        file(COPY "${DIST_BIN}/cef/${relative}"
             DESTINATION "${client}/cef/${subdirectory}")
    endforeach()
else()
    message(WARNING "нет каталога cef: интерфейс у игрока не запустится")
endif()

message(STATUS "разложено: ${client} и ${server}")
