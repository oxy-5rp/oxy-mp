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

# Библиотеки Visual C++, без которых сервер не запустится.
#
# Понадобились они оттого, что сервер собирается с динамической — того требует
# libnode.dll. У клиента их нет и быть не может: он живёт в чужом процессе, и
# Windows ищет ему зависимости рядом с GTA5.exe. Сервер — обычный исполняемый
# файл, и рядом с ним они находятся. Рядом, а не в подкаталоге: зависимости
# исполняемого файла Windows ищет только там — почему это так и чего стоило бы
# обойти, написано в server/CMakeLists.txt.
#
# В отдельном перечне, потому что всего этого может не быть вовсе: сервер
# собирается и без скриптов, и жаловаться на отсутствие того, чего не просили,
# незачем. Копируется то, что нашлось рядом с собранным сервером, — складывает их
# туда сама сборка.
set(server_optional_files
    msvcp140.dll
    vcruntime140.dll
    vcruntime140_1.dll
)

# То, что раскладка клала в корень сервера прежде.
#
# Убирается оттого, что каталог раздачи обновляют поверх прежнего, а не заводят
# заново: не убери мы этого, стомегабайтный libnode.dll остался бы в корне
# навсегда — рядом с собой же, лежащим теперь в modules/js. Убираются только
# файлы, которые клала сюда сама раскладка; чужого она не трогает.
set(server_obsolete_files
    libnode.dll
    msvcp140_1.dll
    msvcp140_2.dll
    msvcp140_atomic_wait.dll
    msvcp140_codecvt_ids.dll
    concrt140.dll
)

foreach(name IN LISTS client_files)
    if(EXISTS "${DIST_BIN}/${name}")
        file(COPY "${DIST_BIN}/${name}" DESTINATION "${client}")
    else()
        message(WARNING "нет ${name}: клиентский каталог будет неполным")
    endif()
endforeach()

# То, что кладут в папку игрока помимо бинарников: подготовка машины,
# сертификат издателя и объяснение для человека.
#
# Подготовка нужна не от прихоти. oxyMP внедряет модуль в процесс игры — так же,
# как RAGE MP и alt:V, — и машинные проверки Защитника Windows считают это
# признаком вредоноса: сборку уносит в карантин прямо на запуске. Спрятать
# поведение нельзя и не нужно; можно один раз сказать системе, что этой папке
# доверяют. Этим «Установить.cmd» и занимается.
#
# Имя по-русски: его читает и запускает человек, а не программа.
set(dist_extras
    "install.cmd:Установить.cmd"
    "setup.ps1:setup.ps1"
    "oxymp.cer:oxymp.cer"
    "readme.txt:ЧИТАЙ_МЕНЯ.txt"
)

foreach(pair IN LISTS dist_extras)
    string(REPLACE ":" ";" parts "${pair}")
    list(GET parts 0 source)
    list(GET parts 1 target)

    set(from "${CMAKE_CURRENT_LIST_DIR}/../launcher/dist/${source}")

    if(EXISTS "${from}")
        # Сертификат необязателен: сборка без подписи его не выпускает, и
        # подготовка тогда просто скажет, что доверять нечему.
        configure_file("${from}" "${client}/${target}" COPYONLY)
    elseif(NOT source STREQUAL "oxymp.cer")
        message(WARNING "нет ${source}: папка клиента будет неполной")
    endif()
endforeach()

# Образец настроек кладётся рядом с сервером, но не поверх уже настроенного:
# перезаписать чужой server.cfg своим образцом — значит стереть работу хозяина
# сервера при первом же обновлении.
if(NOT EXISTS "${server}/server.cfg")
    file(COPY "${CMAKE_CURRENT_LIST_DIR}/../server/server.cfg" DESTINATION "${server}")
endif()

# Ресурсы — по одному и по тому же правилу: свой поверх чужого не кладётся.
#
# Каталог целиком копировать нельзя: рядом с нашими ресурсами лежат чужие, в том
# числе dlcpacks с игровыми файлами хозяина сервера, и слить их одной командой
# значило бы однажды затереть его правку нашим образцом.
file(GLOB sample_resources RELATIVE "${CMAKE_CURRENT_LIST_DIR}/../server/resources"
     "${CMAKE_CURRENT_LIST_DIR}/../server/resources/*")

foreach(name IN LISTS sample_resources)
    if(NOT EXISTS "${server}/resources/${name}")
        file(COPY "${CMAKE_CURRENT_LIST_DIR}/../server/resources/${name}"
             DESTINATION "${server}/resources")
    endif()
endforeach()

foreach(name IN LISTS server_files)
    if(EXISTS "${DIST_BIN}/${name}")
        file(COPY "${DIST_BIN}/${name}" DESTINATION "${server}")
    else()
        message(WARNING "нет ${name}: серверный каталог будет неполным")
    endif()
endforeach()

foreach(name IN LISTS server_optional_files)
    if(EXISTS "${DIST_BIN}/${name}")
        file(COPY "${DIST_BIN}/${name}" DESTINATION "${server}")
    endif()
endforeach()

foreach(name IN LISTS server_obsolete_files)
    if(EXISTS "${server}/${name}")
        file(REMOVE "${server}/${name}")
        message(STATUS "убрано из прежней раскладки: ${name}")
    endif()
endforeach()

# Движок игровых режимов — отдельным каталогом, как у alt:V.
#
# Путь тот же, по которому движок ищет себя сам (script-js/src/node_library.cpp)
# и по которому его кладёт сборка рядом с собранным сервером. Три места, и
# разъехаться им нельзя: разъехавшись, они дают сервер, который собрался,
# разложился и не нашёл движка.
if(EXISTS "${DIST_BIN}/modules")
    file(GLOB_RECURSE module_files RELATIVE "${DIST_BIN}/modules" "${DIST_BIN}/modules/*")

    foreach(relative IN LISTS module_files)
        get_filename_component(subdirectory "${relative}" DIRECTORY)

        file(COPY "${DIST_BIN}/modules/${relative}"
             DESTINATION "${server}/modules/${subdirectory}")
    endforeach()
endif()

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

# Страница меню — файлом рядом с клиентом, а не встроенной в него.
#
# Встроить не выйдет: собранная страница весит четыре с половиной мегабайта, и
# байтовый массив такого размера превратился бы в сотню мегабайт порождённого
# исходника — компилятор такое собирает минутами, если собирает вовсе. Поэтому
# она лежит файлом, а клиент отдаёт её браузеру своей схемой `http://ui/`, как
# это делает alt:V.
if(EXISTS "${DIST_MENU_PAGE}")
    file(COPY "${DIST_MENU_PAGE}" DESTINATION "${client}/ui")
else()
    message(WARNING "нет собранного меню: интерфейс у игрока будет пустым")
endif()

message(STATUS "разложено: ${client} и ${server}")
