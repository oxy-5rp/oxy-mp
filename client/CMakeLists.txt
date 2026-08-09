# Клиентское ядро: соединение с сервером и состояние сессии.
#
# Отдельная библиотека, а не часть DLL, потому что этим же кодом пользуется
# консольный бот-клиент. Сеть таким образом отлаживается без запуска игры.
add_library(oxymp_clientcore STATIC
    src/connection.cpp
)
add_library(oxymp::clientcore ALIAS oxymp_clientcore)

target_include_directories(oxymp_clientcore PUBLIC include)

target_link_libraries(oxymp_clientcore
    PUBLIC oxymp::net oxymp::shared
    PRIVATE oxymp::options spdlog::spdlog
)

target_compile_features(oxymp_clientcore PUBLIC cxx_std_20)
