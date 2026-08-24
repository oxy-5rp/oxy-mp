# Тесты счёта промежуточных состояний.
#
# Единственная часть клиента, которую можно проверить без запущенной игры: она
# не зовёт нативов и ничего не знает ни про машины, ни про персонажей — только
# считает, где между двумя снимками мы находимся сейчас. Именно в этом счёте и
# жила ошибка, из-за которой чужие игроки стояли повёрнутыми не туда.
#
# Второе, что проверяется без игры, — чтение свёртка ресурса. Игра ему тоже не
# нужна: свёрток собирается прямо в проверке, кладётся во временный каталог и
# читается обратно.
add_executable(oxymp_client_tests
    bundle_store_tests.cpp
    interpolation_tests.cpp
    ../src/bundle_store.cpp
    ../src/interpolation.cpp
)

target_link_libraries(oxymp_client_tests PRIVATE
    oxymp::shared
    oxymp::options
    spdlog::spdlog
    Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(oxymp_client_tests)
