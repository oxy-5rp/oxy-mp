// Проверки перевода мест в машине между нумерацией игры и нумерацией alt:V.
//
// Отдельным исполняемым файлом от engine_tests, и это не дробление ради
// дробности: там всякая проверка поднимает Node, а здесь проверяется чистая
// арифметика, которой ни движок, ни ресурс, ни сокет не нужны. Смешай их — и
// счёт двух чисел стоил бы запуска изолята V8.
//
// Без Catch2 по той же причине, что и весь script-js: машина собирается с
// динамической библиотекой времени выполнения, а Catch2 здесь — со статической.
//
// **Проверять это нужно потому, что ошибка здесь молчит.** Обе нумерации
// состоят из законных маленьких чисел, и перепутанные они не дают ни
// исключения, ни строки в журнале — только человека, севшего не на своё место.

#include <oxymp/script/js/alt_seat.hpp>

#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace {

using oxymp::script::js::fromAltSeat;
using oxymp::script::js::kAltDriverSeat;
using oxymp::script::js::toAltSeat;

std::string& failure() {
    static std::string reason;
    return reason;
}

void expect(bool condition, std::string_view what) {
    if (!condition && failure().empty()) {
        failure() = std::string{what};
    }
}

/// Водитель у alt:V — единица, у игры — минус единица.
void aDriverIsSeatOneForAltv() {
    expect(toAltSeat(oxymp::shared::kDriverSeat) == 1, "водитель обязан называться первым местом");
    expect(fromAltSeat(1) == oxymp::shared::kDriverSeat, "первое место alt:V — это место водителя");
}

/// Пассажиры у alt:V идут с двойки, у игры — с нуля.
void passengersStartAtTwoForAltv() {
    expect(toAltSeat(0) == 2, "первый пассажир обязан называться вторым местом");
    expect(toAltSeat(1) == 3, "второй пассажир обязан называться третьим местом");

    expect(fromAltSeat(2) == 0, "второе место alt:V — первый пассажир игры");
    expect(fromAltSeat(3) == 1, "третье место alt:V — второй пассажир игры");
}

/// «Ни в какой машине» — ноль у alt:V и минус двойка у игры.
void beingNowhereIsZeroForAltv() {
    expect(toAltSeat(oxymp::shared::kNoSeat) == 0, "пешего обязан называть ноль");
    expect(fromAltSeat(0) == oxymp::shared::kNoSeat, "ноль alt:V — это «нигде»");
}

/// Перевод туда и обратно возвращает то же самое.
///
/// Проверяется перебором, а не тремя случаями: смещение одно на всю нумерацию, и
/// ошибка в нём проявилась бы на каком-нибудь одном месте, а не на всех сразу.
void translationSurvivesARoundTrip() {
    for (int seat = oxymp::shared::kNoSeat; seat <= 16; ++seat) {
        const auto game = static_cast<std::int8_t>(seat);

        if (fromAltSeat(toAltSeat(game)) != game) {
            failure() = "перевод туда и обратно разошёлся на месте " + std::to_string(seat);
            return;
        }
    }
}

/// Несуразное число из скрипта означает «никуда», а не чужое место.
///
/// Довод приходит из JavaScript, где числом бывает что угодно. Обрежь мы его до
/// байта — и 258 стало бы двойкой, то есть местом водителя.
void nonsenseFromAScriptMeansNowhere() {
    expect(fromAltSeat(258) == oxymp::shared::kNoSeat, "258 не место, а недоразумение");
    expect(fromAltSeat(-5) == oxymp::shared::kNoSeat, "отрицательного места у alt:V нет");
    expect(fromAltSeat(1'000'000) == oxymp::shared::kNoSeat, "миллионного места не бывает");
}

/// Не названное место означает «за руль».
void anUnnamedSeatMeansTheDriver() {
    expect(fromAltSeat(kAltDriverSeat) == oxymp::shared::kDriverSeat,
           "просьба без места обязана сажать за руль");
}

const std::map<std::string, std::function<void()>>& cases() {
    static const std::map<std::string, std::function<void()>> known{
        {"a-driver-is-seat-one-for-altv", &aDriverIsSeatOneForAltv},
        {"passengers-start-at-two-for-altv", &passengersStartAtTwoForAltv},
        {"being-nowhere-is-zero-for-altv", &beingNowhereIsZeroForAltv},
        {"translation-survives-a-round-trip", &translationSurvivesARoundTrip},
        {"nonsense-from-a-script-means-nowhere", &nonsenseFromAScriptMeansNowhere},
        {"an-unnamed-seat-means-the-driver", &anUnnamedSeatMeansTheDriver},
    };

    return known;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("нужно имя проверки. Известные:\n");

        for (const auto& [name, run] : cases()) {
            std::printf("  %s\n", name.c_str());
        }

        return 2;
    }

    const auto found = cases().find(argv[1]);

    if (found == cases().end()) {
        std::printf("нет такой проверки: %s\n", argv[1]);
        return 2;
    }

    found->second();

    if (!failure().empty()) {
        std::printf("%s\n", failure().c_str());
        return 1;
    }

    return 0;
}
