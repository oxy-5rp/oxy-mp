# Подпись одного файла с отметкой времени, устойчивая к чужим неполадкам.
#
# Живёт отдельным сценарием, а не строкой в add_custom_command, ровно затем,
# чтобы уметь повторить попытку. Отметку времени ставит не signtool, а сторонняя
# служба, и служба эта иногда не отвечает — а вместе с ней падает и сборка,
# хотя код собрался целиком. Так и было: `timestamp.digicert.com` не отзывался
# через раз, и вся выпускная сборка кончалась ошибкой шага подписи.
#
# Порядок такой: обойти службы по списку и остановиться на первой ответившей.
# Не ответила ни одна — подписать без отметки и сказать об этом громко. Подпись
# без отметки годна, пока годен сертификат; сборка, упавшая из-за чужого
# сервера, не годна вовсе.
#
# Требовать отметку всё же можно и нужно — тем, что раздают: OXYMP_SIGN_REQUIRE_
# TIMESTAMP=ON превращает эту уступку обратно в ошибку.
#
# Зовётся так:
#   cmake -DSIGNTOOL=... -DSIGN_ARGS=... -DTIMESTAMP_URLS=... -DFILE=...
#         -DREQUIRE_TIMESTAMP=... -P sign_script.cmake

if(NOT SIGNTOOL OR NOT FILE)
    message(FATAL_ERROR "sign_script: не заданы SIGNTOOL или FILE")
endif()

# Списки приходят сюда одной строкой через вертикальную черту: точка с запятой
# внутри -D разошлась бы на отдельные доводы ещё до нас, а разбивать по пробелам
# нельзя — в пути к сертификату пробелы бывают.
string(REPLACE "|" ";" sign_args "${SIGN_ARGS}")
string(REPLACE "|" ";" timestamp_urls "${TIMESTAMP_URLS}")

foreach(url IN LISTS timestamp_urls)
    if(NOT url)
        continue()
    endif()

    execute_process(
        COMMAND "${SIGNTOOL}" sign ${sign_args} /tr "${url}" /td sha256 "${FILE}"
        RESULT_VARIABLE failed
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)

    if(NOT failed)
        return()
    endif()

    message(STATUS "Подпись: служба отметок ${url} не ответила, пробуем следующую")
endforeach()

if(REQUIRE_TIMESTAMP)
    message(FATAL_ERROR
        "Подпись: ни одна служба отметок времени не ответила, а отметка "
        "объявлена обязательной (OXYMP_SIGN_REQUIRE_TIMESTAMP). Последний "
        "ответ signtool:\n${output}")
endif()

# Без отметки. Такая подпись перестанет быть годной в день, когда истечёт
# сертификат, — поэтому предупреждение, а не тишина.
execute_process(
    COMMAND "${SIGNTOOL}" sign ${sign_args} "${FILE}"
    RESULT_VARIABLE failed
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)

if(failed)
    message(FATAL_ERROR "Подпись ${FILE} не удалась:\n${output}")
endif()

message(WARNING
    "Подпись ${FILE} поставлена БЕЗ отметки времени: ни одна служба не "
    "ответила. Такая подпись перестанет быть годной вместе с сертификатом. "
    "Для раздачи пересоберите с работающей службой.")
