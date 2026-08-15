# Модуль, внедряемый в Rockstar Games Launcher.
#
# Отдельная цель, а не часть лаунчера oxyMP: этот код исполняется в чужом
# процессе, и общего с нашим у него — только описание уговора в shared.
add_library(oxymp_rglpatch SHARED
    src/battleye_link.cpp
    src/dllmain.cpp
    src/import_hook.cpp
)

set_target_properties(oxymp_rglpatch PROPERTIES
    OUTPUT_NAME oxymp-rglpatch
    PREFIX ""
)

target_link_libraries(oxymp_rglpatch PRIVATE
    oxymp::options
    oxymp::shared
    spdlog::spdlog
)

oxymp_version_info(oxymp_rglpatch "oxyMP — запуск игры мимо BattlEye")

# Подпись подмены звена BattlEye. Пустышка, пока OXYMP_SIGN выключен.
oxymp_sign(oxymp_rglpatch)
