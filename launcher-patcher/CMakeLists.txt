# Модуль, внедряемый в Rockstar Games Launcher.
#
# Отдельная цель, а не часть лаунчера oxyMP: этот код исполняется в чужом
# процессе, и общего с нашим у него — только описание уговора в shared.
add_library(oxymp_launcher_patcher SHARED
    src/battleye_link.cpp
    src/dllmain.cpp
    src/import_hook.cpp
)

set_target_properties(oxymp_launcher_patcher PROPERTIES
    OUTPUT_NAME oxymp-launcher-patcher
    PREFIX ""
)

target_link_libraries(oxymp_launcher_patcher PRIVATE
    oxymp::options
    oxymp::shared
    spdlog::spdlog
)

oxymp_version_info(oxymp_launcher_patcher "oxyMP — запуск игры мимо BattlEye")

# Подпись подмены звена BattlEye. Пустышка, пока OXYMP_SIGN выключен.
oxymp_sign(oxymp_launcher_patcher)
