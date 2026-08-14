# enet приватно: за пределами этой цели про библиотеку сети никто знать не должен.
oxymp_library(net
    SOURCES src/host.cpp
    INCLUDES include
    PUBLIC oxymp::shared
    PRIVATE enet::enet oxymp::options
)
