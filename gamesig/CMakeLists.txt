# Каталог сигнатур и их разрешение в адреса.
#
# Каталог — чистые данные и живёт в заголовке. Разрешение вынесено сюда, чтобы
# у оффлайн-проверки и у клиента, работающего изнутри игры, она была общей:
# расхождение между «проверено» и «используется» превращало бы отчёт sigcheck
# в пустую бумажку.
add_library(oxymp_gamesig STATIC
    src/resolver.cpp
)
add_library(oxymp::gamesig ALIAS oxymp_gamesig)

target_include_directories(oxymp_gamesig PUBLIC include)

target_link_libraries(oxymp_gamesig
    PUBLIC oxymp::memscan
    PRIVATE oxymp::options
)
