# Интерфейс на движке Edge.
#
# Отдельная библиотека, потому что интерфейсов два и живут они в разных
# процессах: окно подключения в лаунчере и слой поверх игры в webengine.
# Общее у них — сам движок и способ разговаривать со страницей.
add_library(oxymp_webui STATIC
    src/browser.cpp
)
add_library(oxymp::webui ALIAS oxymp_webui)

target_include_directories(oxymp_webui PUBLIC include)

# webview2 приватно: заголовки движка тянут за собой COM и WRL, и всё, что
# подключит наш заголовок, получило бы их следом. Наружу торчит только Browser.
target_link_libraries(oxymp_webui
    PRIVATE oxymp::options webview2::webview2 spdlog::spdlog
)

target_compile_features(oxymp_webui PUBLIC cxx_std_20)
