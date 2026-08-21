# Подпись собранных бинарей сертификатом Authenticode.
#
# Зачем это вообще есть. oxyMP внедряет модуль в игру и правит звено BattlEye —
# то же самое делает вредонос, и поведенческая защита Windows ловит это как
# «уклонение от защиты» (Behavior:Win32/DefenseEvasion). Спрятать это поведение
# нельзя и не нужно: оно настоящее. Единственный честный ответ — не прятать
# программу, а сделать её опознаваемой. Подписанный бинарь известного издателя
# Windows проверяет мягче, а окно «неизвестный издатель» исчезает вовсе.
#
# По умолчанию выключено, и это важно: сборка без сертификата должна идти как
# шла. Подпись — свойство того, что раздают, а не того, что собирают у себя.
#
# Включается так:
#   -DOXYMP_SIGN=ON -DOXYMP_SIGN_THUMBPRINT=<отпечаток из хранилища>
# или сертификатом из файла:
#   -DOXYMP_SIGN=ON -DOXYMP_SIGN_PFX=cert.pfx -DOXYMP_SIGN_PFX_PASSWORD=...
#
# Отпечаток проверочного сертификата выдаёт tools/signing/make-test-cert.ps1 —
# им проверяют, что вся цепочка подписи работает, ещё до покупки настоящего.

option(OXYMP_SIGN "Подписывать собранные бинарники" OFF)

set(OXYMP_SIGN_THUMBPRINT "" CACHE STRING
    "Отпечаток (SHA1) сертификата в хранилище Windows")
set(OXYMP_SIGN_PFX "" CACHE FILEPATH
    "Файл сертификата .pfx — вместо отпечатка из хранилища")
set(OXYMP_SIGN_PFX_PASSWORD "" CACHE STRING
    "Пароль к файлу .pfx")

# Отметка времени берётся у стороннего сервиса, и служб здесь несколько.
#
# Без отметки подпись перестаёт быть годной в тот день, когда истекает
# сертификат, — и уже розданные бинарники разом становятся «недействительно
# подписанными». С отметкой подпись остаётся годной навсегда: сервис заверяет,
# что файл был подписан, пока сертификат ещё действовал.
#
# Список, а не один адрес, потому что служба чужая и иногда молчит. Молчащий
# digicert валил выпускную сборку целиком — при том, что код собирался, а
# подписать было чем. Порядок обхода — порядок списка, и останавливаемся на
# первой ответившей.
set(OXYMP_SIGN_TIMESTAMP_URLS
    "http://timestamp.digicert.com;http://timestamp.sectigo.com;http://timestamp.globalsign.com/tsa/r6advanced1"
    CACHE STRING "Службы отметок времени RFC 3161, в порядке обхода")

# Считать ли отсутствие отметки ошибкой.
#
# Выключено, и это уступка не качеству, а действительности: сборка, упавшая
# из-за чужого сервера, не годна вовсе, а подпись без отметки годна, пока годен
# сертификат. Для того, что раздают, включайте — там отметка обязательна.
option(OXYMP_SIGN_REQUIRE_TIMESTAMP
       "Считать неудачу службы отметок времени ошибкой сборки" OFF)

set(OXYMP_SIGNTOOL "" CACHE FILEPATH
    "Путь к signtool.exe — если поиск в Windows SDK не нашёл нужный")

# Дальше — только когда подпись включена. Иначе модуль не делает ничего, и цель
# oxymp_sign(...) оказывается пустышкой: вызвать её можно всегда, а собирать без
# сертификата ничто не мешает.
if(NOT OXYMP_SIGN)
    function(oxymp_sign target)
    endfunction()

    return()
endif()

if(NOT WIN32)
    message(FATAL_ERROR "OXYMP_SIGN: подпись Authenticode есть только под Windows")
endif()

# Ровно один источник сертификата. Два — это вопрос «которым из них», на который
# ответа нет; ни одного — нечем подписывать.
if(OXYMP_SIGN_THUMBPRINT AND OXYMP_SIGN_PFX)
    message(FATAL_ERROR "OXYMP_SIGN: заданы и отпечаток, и .pfx — оставьте что-то одно")
endif()

if(NOT OXYMP_SIGN_THUMBPRINT AND NOT OXYMP_SIGN_PFX)
    message(FATAL_ERROR
        "OXYMP_SIGN включён, но нечем подписывать: задайте OXYMP_SIGN_THUMBPRINT "
        "или OXYMP_SIGN_PFX (см. tools/signing/make-test-cert.ps1)")
endif()

# Поиск signtool.exe в Windows SDK.
#
# Своего модуля поиска у CMake для него нет, а лежит он в наборе Windows Kits,
# и версий набора на машине бывает несколько. Берётся самый новый x64 — путь
# сортируется по убыванию, и первый найденный и есть нужный.
#
# Корень набора спрашивается у реестра, а не угадывается по Program Files: SDK
# ставят и на другой диск, и тогда его там просто нет. Реестр же знает, куда его
# поставили, на каком бы диске это ни было.
if(OXYMP_SIGNTOOL)
    set(signtool "${OXYMP_SIGNTOOL}")
else()
    cmake_host_system_information(RESULT kits_root
        QUERY WINDOWS_REGISTRY "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots"
        VALUE "KitsRoot10"
        ERROR_VARIABLE kits_error)

    set(signtool_globs
        "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/*/x64/signtool.exe"
        "$ENV{ProgramFiles}/Windows Kits/10/bin/*/x64/signtool.exe")

    # Путь из реестра — первым: он вернее догадок. Обе раскладки набора, старая
    # без версии в пути и новая с версией, потому что встречаются ещё обе.
    if(kits_root)
        list(PREPEND signtool_globs
            "${kits_root}/bin/*/x64/signtool.exe"
            "${kits_root}/bin/x64/signtool.exe"
            "${kits_root}/App Certification Kit/signtool.exe")
    endif()

    file(GLOB signtool_candidates ${signtool_globs})

    if(NOT signtool_candidates)
        message(FATAL_ERROR
            "OXYMP_SIGN: signtool.exe не найден. Он ставится с Windows SDK; "
            "если SDK есть, укажите путь в OXYMP_SIGNTOOL")
    endif()

    list(SORT signtool_candidates ORDER DESCENDING)
    list(GET signtool_candidates 0 signtool)
endif()

message(STATUS "Подпись: ${signtool}")

# Аргументы, общие для всех целей. Отличается только источник ключа.
#
# Хеш файла — SHA-256: SHA-1 давно объявлен негодным, и подпись им современная
# Windows принимает с предупреждением, а то и не принимает вовсе.
#
# Отметка времени сюда не входит: её ставит сценарий подписи, обходя службы по
# списку, и вписать её здесь значило бы лишить его этой возможности.
set(_oxymp_sign_args /fd sha256)

if(OXYMP_SIGN_THUMBPRINT)
    list(PREPEND _oxymp_sign_args /sha1 "${OXYMP_SIGN_THUMBPRINT}")
else()
    list(PREPEND _oxymp_sign_args /f "${OXYMP_SIGN_PFX}")

    if(OXYMP_SIGN_PFX_PASSWORD)
        list(APPEND _oxymp_sign_args /p "${OXYMP_SIGN_PFX_PASSWORD}")
    endif()
endif()

# Подписывает бинарник цели сразу после сборки.
#
# Сразу, а не при раскладке, намеренно: подпись — свойство самого файла, и она
# должна быть при нём везде, куда он попадёт, включая копию, взятую прямо из
# каталога сборки. Раздача подписывала бы только свои копии, а взятый из bin
# лаунчер оказался бы неподписанным — и SmartScreen встретил бы игрока тем же
# «неизвестный издатель», от которого всё и затевалось.
#
# Через сценарий, а не прямым вызовом signtool: тому нечем повторить попытку,
# когда служба отметок времени не ответила, — а не отвечает она регулярно.
# Список служб передаётся через вертикальную черту: точка с запятой внутри
# -D сама разошлась бы на отдельные доводы.
string(REPLACE ";" "|" _oxymp_timestamp_urls "${OXYMP_SIGN_TIMESTAMP_URLS}")
string(JOIN "|" _oxymp_sign_args_line ${_oxymp_sign_args})

function(oxymp_sign target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
                "-DSIGNTOOL=${signtool}"
                "-DSIGN_ARGS=${_oxymp_sign_args_line}"
                "-DTIMESTAMP_URLS=${_oxymp_timestamp_urls}"
                "-DREQUIRE_TIMESTAMP=${OXYMP_SIGN_REQUIRE_TIMESTAMP}"
                "-DFILE=$<TARGET_FILE:${target}>"
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/sign_script.cmake"
        COMMENT "Подпись ${target}"
        VERBATIM)
endfunction()
