add_executable(oxymp_sigcheck
    src/main.cpp
    src/pe_image.cpp
)

set_target_properties(oxymp_sigcheck PROPERTIES OUTPUT_NAME sigcheck)

target_link_libraries(oxymp_sigcheck PRIVATE
    oxymp::memscan
    oxymp::gamesig
    oxymp::options
)
