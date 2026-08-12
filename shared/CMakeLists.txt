add_library(oxymp_shared STATIC
    src/messages.cpp
    src/serialization.cpp
    src/vault.cpp
)
add_library(oxymp::shared ALIAS oxymp_shared)

target_include_directories(oxymp_shared PUBLIC include)
target_link_libraries(oxymp_shared PRIVATE oxymp::options)
target_compile_features(oxymp_shared PUBLIC cxx_std_20)
