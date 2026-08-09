add_library(oxymp_net STATIC
    src/host.cpp
)
add_library(oxymp::net ALIAS oxymp_net)

target_include_directories(oxymp_net PUBLIC include)

# enet приватно: за пределами этой цели про библиотеку сети никто знать не должен.
target_link_libraries(oxymp_net
    PUBLIC oxymp::shared
    PRIVATE enet::enet oxymp::options
)

target_compile_features(oxymp_net PUBLIC cxx_std_20)
