// Проверки чтения справочников моделей.
//
// Файл этот кладёт рядом с сервером человек, и оборванный на середине он ничем
// не отличается от целого, пока не прочтёшь. Поэтому проверяется здесь не
// столько правильное чтение, сколько **отказ**: неполный файл, чужой файл и
// файл другого выпуска обязаны быть отвергнуты целиком, а не разобраны
// наполовину.
//
// Разница между «этой модели нет в игре» и «файл оборвался» изнутри режима не
// видна никак: и там, и там ответ пустой. Оттого половина прочитанного хуже, чем
// ничего.

#include "../src/game_data.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace {

using oxymp::server::GameData;

/// Складывает файл справочника из кусков.
class Builder {
public:
    Builder& u8(std::uint8_t value) {
        bytes_.push_back(value);
        return *this;
    }

    Builder& u16(std::uint16_t value) {
        bytes_.push_back(static_cast<std::uint8_t>(value & 0xFFU));
        bytes_.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
        return *this;
    }

    Builder& u32(std::uint32_t value) {
        u16(static_cast<std::uint16_t>(value & 0xFFFFU));
        u16(static_cast<std::uint16_t>((value >> 16U) & 0xFFFFU));
        return *this;
    }

    Builder& text(std::string_view value) {
        u16(static_cast<std::uint16_t>(value.size()));

        for (const char symbol : value) {
            bytes_.push_back(static_cast<std::uint8_t>(symbol));
        }

        return *this;
    }

    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
};

/// Целый справочник из одной машины, одного человека, одного оружия и одного
/// набора тюнинга. Больше и не нужно: разбор идёт по одной раскладке.
[[nodiscard]] Builder wholeFile() {
    Builder out;

    out.u8('O').u8('X').u8('G').u8('D').u16(1);

    // Словарь строк.
    out.u32(4).text("sultan").text("basegame").text("chassis").text("SKEL_Head");

    // Машины.
    out.u32(1);
    out.u32(0x3404691C);                       // хеш
    out.u32(0);                                // название -> "sultan"
    out.u8(2).u8(4).u8(0);                     // род, колёса, бронестёкла
    out.u8(12).u8(0).u8(0).u8(156).u8(0).u8(0); // цвета
    out.u16(45).u16(0xFFFF);                   // наборы тюнинга
    out.u16(6).u16(2);                         // дополнения кузова
    out.u8(0).u8(0);                           // прицеп, машины
    out.u32(0x3404691C);                       // управляемость
    out.u32(1);                                // набор -> "basegame"
    out.u16(1).u32(0).u32(2);                  // одна кость -> "chassis"

    // Люди.
    out.u32(1);
    out.u32(0x705E61F2);
    out.u32(0).u32(1).u32(1).u32(1).u32(1);
    out.u16(1).u16(7).u16(0).u32(3);           // одна кость -> "SKEL_Head"

    // Оружие.
    out.u32(1);
    out.u32(0x1B06D571);
    out.u32(0).u32(0).u32(0x577AAB2D).u32(0).u32(1).u32(1).u32(0);
    out.u32(250).u32(1000).u32(9999).u32(0).u32(1);

    // Наборы тюнинга.
    out.u32(1);
    out.u16(45).u32(0);
    out.u8(1);
    out.u8(11).u8(4).u16(0).u16(1).u16(2).u16(3);

    return out;
}

/// Кладёт байты во временный файл и убирает его за собой.
class Scratch {
public:
    explicit Scratch(const std::vector<std::uint8_t>& bytes) {
        static std::mt19937_64 tokens{std::random_device{}()};

        path_ = std::filesystem::temp_directory_path() /
                ("oxymp-gamedata-" + std::to_string(tokens()) + ".bin");

        std::ofstream file{path_, std::ios::binary};
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }

    ~Scratch() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("a whole game data file is read to the last byte", "[server][gamedata]") {
    const Scratch file{wholeFile().bytes()};

    GameData models;
    std::string failure;

    REQUIRE(models.load(file.path(), failure));
    CHECK(failure.empty());
    CHECK(models.loaded());

    const auto* car = models.vehicle(0x3404691C);

    REQUIRE(car != nullptr);
    CHECK(car->title == "sultan");
    CHECK(car->wheelsCount == 4);
    CHECK(car->primaryColour == 12);
    CHECK(car->modKit == 45);
    CHECK(car->handlingNameHash == 0x3404691C);
    REQUIRE(car->bones.size() == 1);
    CHECK(car->bones[0].name == "chassis");

    const auto* person = models.ped(0x705E61F2);

    REQUIRE(person != nullptr);
    REQUIRE(person->bones.size() == 1);
    CHECK(person->bones[0].id == 7);
    CHECK(person->bones[0].name == "SKEL_Head");

    const auto* gun = models.weapon(0x1B06D571);

    REQUIRE(gun != nullptr);
    CHECK(gun->modelHash == 0x577AAB2D);
    CHECK(gun->defaultMaxAmmo == 250);
}

TEST_CASE("a missing game data file is not a failure", "[server][gamedata]") {
    // Справочник необязателен, и жаловаться на то, чего никто не обещал, значит
    // пугать хозяина сервера впустую. Поломка — это файл, который есть и не
    // читается.
    GameData models;
    std::string failure;

    CHECK(models.load("такого-файла-нет.bin", failure));
    CHECK(failure.empty());
    CHECK_FALSE(models.loaded());
}

TEST_CASE("a truncated game data file is refused whole", "[server][gamedata]") {
    // Половина прочитанного хуже, чем ничего: разницу между «этой модели нет в
    // игре» и «файл оборвался» изнутри режима не видно никак.
    std::vector<std::uint8_t> bytes = wholeFile().bytes();
    bytes.resize(bytes.size() / 2);

    const Scratch file{bytes};

    GameData models;
    std::string failure;

    CHECK_FALSE(models.load(file.path(), failure));
    CHECK_FALSE(failure.empty());
    CHECK_FALSE(models.loaded());

    // Ничего не осталось и от прочитанного до обрыва.
    CHECK(models.vehicle(0x3404691C) == nullptr);
}

TEST_CASE("a game data file with a leftover tail is refused", "[server][gamedata]") {
    // Лишний хвост означает, что раскладка разошлась, а сошедшиеся до него числа
    // оказались не теми. Правдоподобные числа не на своих местах — худший из
    // возможных исходов: ошибки нет, а ответы неверны.
    std::vector<std::uint8_t> bytes = wholeFile().bytes();
    bytes.push_back(0);

    const Scratch file{bytes};

    GameData models;
    std::string failure;

    CHECK_FALSE(models.load(file.path(), failure));
    CHECK_FALSE(models.loaded());
}

TEST_CASE("a game data file of another kind is refused", "[server][gamedata]") {
    std::vector<std::uint8_t> bytes = wholeFile().bytes();
    bytes[0] = 'V';

    const Scratch file{bytes};

    GameData models;
    std::string failure;

    CHECK_FALSE(models.load(file.path(), failure));
    CHECK_FALSE(models.loaded());
}

TEST_CASE("a game data file of another version is refused", "[server][gamedata]") {
    // Точным равенством, а не «не старше»: раскладка здесь сплошная, без
    // оглавления, и файл другого выпуска разобрался бы не как ошибка, а как
    // чепуха.
    std::vector<std::uint8_t> bytes = wholeFile().bytes();
    bytes[4] = 9;

    const Scratch file{bytes};

    GameData models;
    std::string failure;

    CHECK_FALSE(models.load(file.path(), failure));
    CHECK_FALSE(models.loaded());
}

TEST_CASE("knowing a model with no modkit is not the same as not knowing the model",
          "[server][gamedata]") {
    // Ноль означает «в этом месте тюнинга не бывает», минус единица — «про эту
    // модель мы ничего не знаем». Показывать их одинаково нельзя: первое это
    // пустое меню, второе — отсутствующий справочник.
    const Scratch file{wholeFile().bytes()};

    GameData models;
    std::string failure;

    REQUIRE(models.load(file.path(), failure));

    CHECK(models.modsCount(0x3404691C, 11) == 4);
    CHECK(models.modsCount(0x3404691C, 12) == 0);
    CHECK(models.modsCount(0xDEADBEEF, 11) == -1);
}
