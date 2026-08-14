// Имена тестов латиницей: ctest передаёт их обратно в исполняемый файл как
// фильтр, и не-ASCII имена ломаются о кодировку консоли Windows.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../src/interpolation.hpp"

using namespace oxymp::client::interpolation;
using Catch::Approx;

namespace {

/// Промежуток между снимками по часам отправителя.
std::chrono::milliseconds span(int milliseconds) {
    return std::chrono::milliseconds{milliseconds};
}

/// Сколько прошло у нас с прихода последнего снимка.
std::chrono::milliseconds since(int milliseconds) {
    return std::chrono::milliseconds{milliseconds};
}

} // namespace

TEST_CASE("blend sits between the two snapshots", "[interpolation]") {
    // Снимки отстоят на 100 мс по часам отправителя, а с прихода последнего у
    // нас прошло 50 мс. Отставание — 100 мс, значит показывать нужно мгновение
    // за 50 мс до последнего снимка, то есть ровно середину между ними.
    const auto result = blend(span(100), since(50));

    CHECK(result.ahead == 0.0F);
    CHECK(result.progress == Approx(0.5F));
}

TEST_CASE("blend stops at the latest snapshot", "[interpolation]") {
    // Последний снимок пришёл ровно kDelay назад: доля пути полная, достраивать
    // нечего.
    const auto result = blend(span(100), since(100));

    CHECK(result.ahead == 0.0F);
    CHECK(result.progress == Approx(1.0F));
}

TEST_CASE("blend ignores how late a snapshot arrived", "[interpolation]") {
    // Главное свойство расчёта и причина, по которой отметка времени вообще
    // появилась. Снимки уходят ровно через 100 мс, а приходят как придётся.
    // Показываемое мгновение зависит только от того, сколько мы уже показываем
    // этот отрезок, — длину отрезка задаёт отправитель, а не сеть.
    //
    // Раньше длиной отрезка служила разница прихода, и запоздавший пакет сжимал
    // само движение: чужая машина то замедлялась, то дёргалась вперёд.
    const auto onTime = blend(span(100), since(50));
    const auto late = blend(span(100), since(50));

    CHECK(onTime.progress == Approx(late.progress));

    // А вот более длинный промежуток на том же месте даёт другую долю — и это
    // верно: за те же 50 мс пройдена меньшая часть более длинного отрезка.
    const auto longer = blend(span(200), since(50));

    CHECK(longer.progress == Approx(0.75F));
    CHECK(longer.progress > onTime.progress);
}

TEST_CASE("blend extrapolates when snapshots stop coming", "[interpolation]") {
    // С прихода последнего снимка прошло 250 мс при отставании в 100: свежих
    // нет полтораста миллисекунд, и движение приходится достраивать. Предел
    // достраивания при этом не задет — иначе проверялся бы он, а не расчёт.
    const auto result = blend(span(100), since(250));

    CHECK(result.ahead == Approx(0.15F));
}

TEST_CASE("blend refuses to extrapolate forever", "[interpolation]") {
    // Связь пропала надолго. Без предела чужой игрок уехал бы в бесконечность
    // по последней известной скорости.
    const auto result = blend(span(100), since(60'000));

    const float limit = std::chrono::duration<float>{kMaxExtrapolation}.count();
    CHECK(result.ahead == Approx(limit));
}

TEST_CASE("blend survives a single snapshot", "[interpolation]") {
    // Первый снимок игрока: предыдущего нет, промежуток нулевой. Делить на него
    // нельзя, а показать игрока нужно — по этому единственному снимку.
    const auto result = blend(span(0), since(50));

    CHECK(result.ahead == 0.0F);
    CHECK(result.progress == Approx(1.0F));
}

TEST_CASE("catching up does not depend on the frame rate", "[interpolation]") {
    // Главное свойство и причина, по которой расчёт вообще появился. Раньше за
    // кадр съедалась постоянная доля расхождения, и при шестидесяти кадрах оно
    // закрывалось вдвое быстрее, чем при тридцати: одна и та же машина у двух
    // зрителей ехала по-разному, и виновата в этом была не сеть.
    constexpr float kRate = 26.0F;

    // Что осталось от расхождения за секунду при тридцати кадрах и при
    // шестидесяти. Ответ обязан быть одним и тем же.
    float slow = 1.0F;
    for (int frame = 0; frame < 30; ++frame) {
        slow *= 1.0F - catchUp(kRate, 1.0F / 30.0F);
    }

    float fast = 1.0F;
    for (int frame = 0; frame < 60; ++frame) {
        fast *= 1.0F - catchUp(kRate, 1.0F / 60.0F);
    }

    CHECK(slow == Approx(fast).margin(1e-6F));
}

TEST_CASE("catching up stays within its bounds", "[interpolation]") {
    // Времени не прошло — закрывать нечего. Кадр нулевой длины случается на
    // самом первом, когда сравнивать не с чем.
    CHECK(catchUp(26.0F, 0.0F) == 0.0F);

    // И назад время не идёт: счётчик игры переполняется, и разница уходит в
    // минус. Отрицательная доля тянула бы тело прочь от снимка.
    CHECK(catchUp(26.0F, -1.0F) == 0.0F);

    // Долгий кадр закрывает расхождение почти целиком, но не больше него:
    // доля свыше единицы означала бы промах мимо цели с перелётом.
    CHECK(catchUp(26.0F, 10.0F) <= 1.0F);
    CHECK(catchUp(26.0F, 10.0F) == Approx(1.0F));
}

TEST_CASE("angles turn the short way around", "[interpolation]") {
    // Главное свойство расчёта углов, и то самое, из-за отсутствия которого
    // чужие игроки разворачивались не в ту сторону: с 350 градусов на 10 ближе
    // идти через ноль, а не через всю окружность обратно.
    CHECK(mixAngle(350.0F, 10.0F, 0.5F) == Approx(0.0F));
    CHECK(mixAngle(10.0F, 350.0F, 0.5F) == Approx(0.0F));

    // И то же в обычном случае, без перехода через ноль.
    CHECK(mixAngle(0.0F, 90.0F, 0.5F) == Approx(45.0F));
}

TEST_CASE("angle blending keeps its ends", "[interpolation]") {
    // Доля ноль — начало, доля единица — конец. Иначе поворот приходил бы не
    // туда, куда смотрит хозяин, а рядом.
    CHECK(mixAngle(30.0F, 200.0F, 0.0F) == Approx(30.0F));
    CHECK(mixAngle(30.0F, 200.0F, 1.0F) == Approx(-160.0F)); // те же 200 градусов

    // Угол приводится к промежутку от минус половины оборота до половины: игра
    // принимает и такой, а сравнивать их между собой так проще.
    CHECK(mixAngle(180.0F, 180.0F, 0.5F) == Approx(-180.0F));
}

TEST_CASE("angular velocity comes in radians", "[interpolation]") {
    // Поворот игра отдаёт в градусах, а угловую скорость — в радианах в
    // секунду. Сложи их без пересчёта — и машина за секунду довернётся на
    // пятьдесят семь градусов вместо одного.
    const auto turned = advanceAngles(oxymp::shared::Vec3{0.0F, 0.0F, 0.0F},
                                      oxymp::shared::Vec3{0.0F, 0.0F, 1.0F}, 1.0F);

    CHECK(turned.z == Approx(57.2957F).margin(0.01F));
}

TEST_CASE("positions move along their velocity", "[interpolation]") {
    const auto moved = advance(oxymp::shared::Vec3{10.0F, 0.0F, 5.0F},
                               oxymp::shared::Vec3{2.0F, -4.0F, 0.0F}, 0.5F);

    CHECK(moved.x == Approx(11.0F));
    CHECK(moved.y == Approx(-2.0F));
    CHECK(moved.z == Approx(5.0F));
}
