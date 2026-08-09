add_library(oxymp_memscan STATIC
    src/pattern.cpp
    src/scanner.cpp
)
add_library(oxymp::memscan ALIAS oxymp_memscan)

target_include_directories(oxymp_memscan PUBLIC include)
target_link_libraries(oxymp_memscan PRIVATE oxymp::options)

# Заголовки публичные, значит требования стандарта тоже должны уехать наружу.
target_compile_features(oxymp_memscan PUBLIC cxx_std_20)
