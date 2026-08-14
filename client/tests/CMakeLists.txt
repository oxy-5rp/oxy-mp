# Тесты счёта промежуточных состояний.
#
# Единственная часть клиента, которую можно проверить без запущенной игры: она
# не зовёт нативов и ничего не знает ни про машины, ни про персонажей — только
# считает, где между двумя снимками мы находимся сейчас. Именно в этом счёте и
# жила ошибка, из-за которой чужие игроки стояли повёрнутыми не туда.
add_executable(oxymp_client_tests
    interpolation_tests.cpp
    ../src/interpolation.cpp
)

target_link_libraries(oxymp_client_tests PRIVATE
    oxymp::shared
    oxymp::options
    Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(oxymp_client_tests)
