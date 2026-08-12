# Интерфейс на CEF: страница, нарисованная в память, а не в окно.
#
# Ради этого CEF и заведён. WebView2 умеет отдавать страницу только в окно, и
# чтобы попасть в кадр игры, её приходилось снимать оттуда средствами Windows —
# работало, но оставалось обходным путём. CEF отдаёт готовые точки прямо в
# OnPaint, с настоящей прозрачностью и без всякого окна.

# --- Обёртка CEF ---------------------------------------------------------
#
# Собирается из исходников, и иначе нельзя: с libcef.dll она разговаривает по
# языку C, а наружу торчит классами C++. Собранная чужим компилятором и с чужой
# стандартной библиотекой, она не подошла бы никому.
file(GLOB_RECURSE OXYMP_CEF_WRAPPER_SOURCES CONFIGURE_DEPENDS
    "${OXYMP_CEF_ROOT}/libcef_dll/*.cc"
)

add_library(oxymp_cefwrapper STATIC ${OXYMP_CEF_WRAPPER_SOURCES})

target_include_directories(oxymp_cefwrapper SYSTEM PUBLIC "${OXYMP_CEF_ROOT}")

target_compile_definitions(oxymp_cefwrapper PUBLIC
    WRAPPING_CEF_SHARED
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
)

target_compile_features(oxymp_cefwrapper PUBLIC cxx_std_20)

if(MSVC)
    # Чужой код собирается без наших предупреждений: править его мы всё равно не
    # станем, а разбирать чужие замечания в своей сборке — значит не заметить
    # своих. /wd4100 и прочее не перечисляются поимённо намеренно.
    target_compile_options(oxymp_cefwrapper PRIVATE /W0 /utf-8 /EHsc)
endif()

# Библиотека-заглушка libcef подключается с отложенной загрузкой.
#
# Это обязательное условие: клиент внедряется в чужой процесс, и Windows искала
# бы libcef.dll рядом с GTA5.exe — то есть в папке игры, куда oxyMP не кладёт
# ничего. С отложенной загрузкой первый вызов происходит уже после того, как мы
# сами загрузили библиотеку из своего каталога.
add_library(oxymp_cef INTERFACE)
add_library(oxymp::cef ALIAS oxymp_cef)

target_link_libraries(oxymp_cef INTERFACE
    oxymp_cefwrapper
    "${OXYMP_CEF_ROOT}/Release/libcef.lib"
)

target_link_options(oxymp_cef INTERFACE /DELAYLOAD:libcef.dll)

# --- Наша обвязка --------------------------------------------------------

add_library(oxymp_cefui STATIC
    src/browser.cpp
    src/runtime.cpp
)
add_library(oxymp::cefui ALIAS oxymp_cefui)

target_include_directories(oxymp_cefui PUBLIC include)

# CEF приватно: его заголовки тянут за собой половину Chromium, и всё, что
# подключит наш заголовок, получило бы их следом. Наружу торчит только Browser.
target_link_libraries(oxymp_cefui
    PRIVATE oxymp::options oxymp::cef spdlog::spdlog
)

target_compile_features(oxymp_cefui PUBLIC cxx_std_20)

# --- Подпроцессы ---------------------------------------------------------
#
# Chromium работает несколькими процессами, и это не выбор: отрисовка страницы,
# графика и сеть живут в отдельных, чтобы падение одного не уносило остальные.
# Здесь важно другое — подпроцессом не может быть GTA5.exe, в который внедрён
# клиент. Поэтому заводится свой крошечный исполняемый файл, который только и
# делает, что передаёт управление CEF.
add_executable(oxymp_cefsub WIN32 src/subprocess.cpp)

set_target_properties(oxymp_cefsub PROPERTIES
    OUTPUT_NAME oxymp-cefsub
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/cef"
)

target_link_libraries(oxymp_cefsub PRIVATE oxymp::options oxymp::cef)

# --- Хозяйство CEF рядом с исполняемыми файлами ---------------------------
#
# Кладётся в тот же каталог cef, где уже живёт слой загрузки: с точки зрения
# игрока это одно и то же — «внутренности интерфейса», и разносить их по двум
# папкам незачем.
set(OXYMP_CEF_STAGE "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/cef")

add_custom_target(oxymp_cef_runtime ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${OXYMP_CEF_STAGE}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${OXYMP_CEF_ROOT}/Release" "${OXYMP_CEF_STAGE}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${OXYMP_CEF_ROOT}/Resources" "${OXYMP_CEF_STAGE}"
    # Библиотека-заглушка нужна только сборке: рядом с игроком ей делать нечего.
    COMMAND ${CMAKE_COMMAND} -E rm -f "${OXYMP_CEF_STAGE}/libcef.lib"
    COMMENT "Раскладка хозяйства CEF"
    VERBATIM
)
