# Каталог сигнатур — это чистые данные, поэтому цель заголовочная.
# Ею пользуются и оффлайн-утилита sigcheck, и клиент, работающий с живым процессом.
add_library(oxymp_gamesig INTERFACE)
add_library(oxymp::gamesig ALIAS oxymp_gamesig)

target_include_directories(oxymp_gamesig INTERFACE include)
target_compile_features(oxymp_gamesig INTERFACE cxx_std_20)
