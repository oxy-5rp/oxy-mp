#include "../src/game/native_context.hpp"

#include <oxymp/shared/protocol/messages.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>

using oxymp::client::game::NativeContext;
using oxymp::shared::kDriverSeat;

namespace {

/// Как игра прочтёт довод, если ждёт от него обычное целое.
///
/// Ячейка у неё восьмибайтная, а `int` из неё она берёт младшими четырьмя.
/// Проверки написаны через это чтение, а не через саму ячейку: сравнивать
/// осмысленно именно то, что увидит натив.
[[nodiscard]] std::int32_t asInt(const NativeContext& context, std::size_t index) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(context.argument(index)));
}

} // namespace

TEST_CASE("a driver seat stays a driver seat on the way into the game", "[native]") {
    // Место водителя в GTA — это -1, и живёт оно в протоколе однобайтовым
    // числом. Уложенное побайтно в обнулённую ячейку, оно превращалось в 255:
    // `GET_PED_IN_VEHICLE_SEAT` спрашивался про место, которого нет ни у одной
    // машины, и водителя не опознавал никогда.
    NativeContext context;
    context.push(kDriverSeat);

    CHECK(asInt(context, 0) == -1);
}

TEST_CASE("a narrow number keeps its sign whatever its width", "[native]") {
    NativeContext context;

    context.push(static_cast<std::int8_t>(-1));
    context.push(static_cast<std::int16_t>(-1000));
    context.push(static_cast<std::int32_t>(-70000));
    context.push(static_cast<std::int64_t>(-5));

    CHECK(asInt(context, 0) == -1);
    CHECK(asInt(context, 1) == -1000);
    CHECK(asInt(context, 2) == -70000);
    CHECK(asInt(context, 3) == -5);
}

TEST_CASE("an unsigned number is widened with zeroes", "[native]") {
    // Хеши моделей и оружия — беззнаковые и занимают все тридцать два разряда.
    // Расширь мы их со знаком, всякий хеш выше двух миллиардов уехал бы в игру
    // отрицательным.
    NativeContext context;

    context.push(static_cast<std::uint8_t>(255));
    context.push(static_cast<std::uint32_t>(0xF00DBEEFU));

    CHECK(context.argument(0) == 255U);
    CHECK(context.argument(1) == 0xF00DBEEFU);
}

TEST_CASE("a fractional number reaches the game as four bytes", "[native]") {
    // Координаты и углы игра читает тридцатидвухразрядным дробным. Превращать
    // его во что-либо нельзя: ячейка обнулена, и четырёх байт довольно.
    NativeContext context;
    context.push(-12.5F);

    float back = 0.0F;
    const std::uint32_t bits = static_cast<std::uint32_t>(context.argument(0));
    std::memcpy(&back, &bits, sizeof(back));

    CHECK(back == -12.5F);
    CHECK((context.argument(0) >> 32) == 0);
}

TEST_CASE("a boolean reaches the game as one or zero", "[native]") {
    NativeContext context;

    context.push(true);
    context.push(false);

    CHECK(context.argument(0) == 1U);
    CHECK(context.argument(1) == 0U);
}

TEST_CASE("arguments are counted as they are laid out", "[native]") {
    NativeContext context;

    CHECK(context.argumentCount() == 0);

    context.push(1);
    context.push(2);

    CHECK(context.argumentCount() == 2);
}
