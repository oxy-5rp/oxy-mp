#include "game_data.hpp"

#include <cstring>
#include <format>
#include <fstream>
#include <iterator>

namespace oxymp::server {
namespace {

constexpr std::array<char, 4> kMagic{'O', 'X', 'G', 'D'};

/// Какой выпуск файла мы умеем читать.
///
/// Сверяется точным равенством, а не «не старше»: раскладка здесь сплошная, без
/// оглавления, и файл другого выпуска разобрался бы не как ошибка, а как чепуха
/// — с правдоподобными числами не на своих местах.
constexpr std::uint16_t kVersion = 1;

/// Чтение по порядку с проверкой края на каждом шаге.
///
/// Край проверяется всегда, а не в отладочной сборке: файл этот кладёт рядом с
/// сервером человек, и оборванный на середине он ничем не отличается от целого,
/// пока не прочтёшь. Чтение за краем — это чужая память, а не пустое значение.
class Reader {
public:
    explicit Reader(const std::vector<std::uint8_t>& data) noexcept : data_(data) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }

    [[nodiscard]] std::uint8_t u8() {
        if (!room(1)) {
            return 0;
        }

        return data_[at_++];
    }

    [[nodiscard]] std::uint16_t u16() {
        if (!room(2)) {
            return 0;
        }

        std::uint16_t value = 0;
        std::memcpy(&value, data_.data() + at_, sizeof(value));
        at_ += sizeof(value);
        return value;
    }

    [[nodiscard]] std::uint32_t u32() {
        if (!room(4)) {
            return 0;
        }

        std::uint32_t value = 0;
        std::memcpy(&value, data_.data() + at_, sizeof(value));
        at_ += sizeof(value);
        return value;
    }

    [[nodiscard]] std::string text(std::uint16_t length) {
        if (!room(length)) {
            return {};
        }

        std::string value(reinterpret_cast<const char*>(data_.data() + at_), length);
        at_ += length;
        return value;
    }

    [[nodiscard]] bool at(std::size_t offset) const noexcept { return at_ == offset; }

private:
    [[nodiscard]] bool room(std::size_t bytes) {
        if (at_ + bytes > data_.size()) {
            ok_ = false;
            return false;
        }

        return ok_;
    }

    const std::vector<std::uint8_t>& data_;
    std::size_t at_ = 0;
    bool ok_ = true;
};

} // namespace

bool GameData::load(const std::filesystem::path& path, std::string& error) {
    std::error_code failure;

    if (!std::filesystem::exists(path, failure)) {
        // Не поломка: справочник необязателен. Молчание здесь верное — жаловаться
        // на то, чего никто не обещал, значит пугать хозяина сервера впустую.
        return true;
    }

    std::ifstream file{path, std::ios::binary};

    if (!file) {
        error = std::format("{} is there but cannot be opened", path.string());
        return false;
    }

    const std::vector<std::uint8_t> data{std::istreambuf_iterator<char>{file},
                                         std::istreambuf_iterator<char>{}};

    Reader reader{data};

    for (const char expected : kMagic) {
        if (static_cast<char>(reader.u8()) != expected) {
            error = std::format("{} is not an oxyMP game data file", path.string());
            return false;
        }
    }

    if (const std::uint16_t version = reader.u16(); version != kVersion) {
        error = std::format("{}: game data version {}, this server reads {}", path.string(),
                            version, kVersion);
        return false;
    }

    // Словарь строк. Всё, что дальше, ссылается на него номером — оттого файл и
    // вдвое меньше исходных: имя кости `SKEL_Head` лежит в нём один раз, а не по
    // разу на каждую из тысячи с лишним моделей людей.
    std::vector<std::string> pool;
    const std::uint32_t poolSize = reader.u32();
    pool.reserve(poolSize);

    for (std::uint32_t at = 0; at < poolSize && reader.ok(); ++at) {
        const std::uint16_t length = reader.u16();
        pool.push_back(reader.text(length));
    }

    const auto word = [&pool](std::uint32_t index) -> std::string {
        return index < pool.size() ? pool[index] : std::string{};
    };

    const std::uint32_t vehicleCount = reader.u32();

    for (std::uint32_t at = 0; at < vehicleCount && reader.ok(); ++at) {
        script::VehicleModelInfo one;

        one.modelHash = reader.u32();
        one.title = word(reader.u32());
        one.type = reader.u8();
        one.wheelsCount = reader.u8();
        one.hasArmouredWindows = reader.u8() != 0;
        one.primaryColour = reader.u8();
        one.secondaryColour = reader.u8();
        one.pearlColour = reader.u8();
        one.wheelColour = reader.u8();
        one.interiorColour = reader.u8();
        one.dashboardColour = reader.u8();
        one.modKit = reader.u16();
        one.secondModKit = reader.u16();
        one.extras = reader.u16();
        one.defaultExtras = reader.u16();
        one.hasAutoAttachTrailer = reader.u8() != 0;
        one.canAttachCars = reader.u8() != 0;
        one.handlingNameHash = reader.u32();
        one.dlc = word(reader.u32());

        const std::uint16_t bones = reader.u16();
        one.bones.reserve(bones);

        for (std::uint16_t which = 0; which < bones && reader.ok(); ++which) {
            script::BoneInfo bone;
            bone.id = reader.u32();
            bone.index = which;
            bone.name = word(reader.u32());
            one.bones.push_back(std::move(bone));
        }

        const std::uint32_t key = one.modelHash;
        vehicles_.emplace(key, std::move(one));
    }

    const std::uint32_t pedCount = reader.u32();

    for (std::uint32_t at = 0; at < pedCount && reader.ok(); ++at) {
        script::PedModelInfo one;

        one.hash = reader.u32();
        one.name = word(reader.u32());
        one.type = word(reader.u32());
        one.dlc = word(reader.u32());
        one.defaultUnarmedWeapon = word(reader.u32());
        one.movementClipSet = word(reader.u32());

        const std::uint16_t bones = reader.u16();
        one.bones.reserve(bones);

        for (std::uint16_t which = 0; which < bones && reader.ok(); ++which) {
            script::BoneInfo bone;
            bone.id = reader.u16();
            bone.index = reader.u16();
            bone.name = word(reader.u32());
            one.bones.push_back(std::move(bone));
        }

        const std::uint32_t key = one.hash;
        peds_.emplace(key, std::move(one));
    }

    const std::uint32_t weaponCount = reader.u32();

    for (std::uint32_t at = 0; at < weaponCount && reader.ok(); ++at) {
        script::WeaponModelInfo one;

        one.hash = reader.u32();
        one.name = word(reader.u32());
        one.modelName = word(reader.u32());
        one.modelHash = reader.u32();
        one.ammoTypeHash = reader.u32();
        one.ammoType = word(reader.u32());
        one.ammoModelName = word(reader.u32());
        one.ammoModelHash = reader.u32();
        one.defaultMaxAmmo = reader.u32();
        one.skillAbove50MaxAmmo = reader.u32();
        one.maxSkillMaxAmmo = reader.u32();
        one.bonusMaxAmmo = reader.u32();
        one.damageType = word(reader.u32());

        const std::uint32_t key = one.hash;
        weapons_.emplace(key, std::move(one));
    }

    const std::uint32_t kitCount = reader.u32();

    for (std::uint32_t at = 0; at < kitCount && reader.ok(); ++at) {
        const std::uint16_t id = reader.u16();
        (void)reader.u32(); // Имя набора нам ни к чему: спрашивают у него счёт.

        std::unordered_map<std::uint8_t, std::uint8_t> slots;
        const std::uint8_t slotCount = reader.u8();

        for (std::uint8_t which = 0; which < slotCount && reader.ok(); ++which) {
            const std::uint8_t slot = reader.u8();
            const std::uint8_t mods = reader.u8();

            slots.emplace(slot, mods);

            for (std::uint8_t skip = 0; skip < mods && reader.ok(); ++skip) {
                (void)reader.u16();
            }
        }

        kits_.emplace(id, std::move(slots));
    }

    if (!reader.ok()) {
        error = std::format("{} ends in the middle: the file is truncated or not ours",
                            path.string());

        // Прочитанное до обрыва выбрасывается целиком. Оставить половину значило
        // бы отвечать правдой про одни модели и «не знаю» про другие — а разницу
        // между «этой модели нет в игре» и «файл оборвался» изнутри режима не
        // видно никак.
        vehicles_.clear();
        peds_.clear();
        weapons_.clear();
        kits_.clear();
        return false;
    }

    // Лишний хвост — тоже поломка: он означает, что раскладка разошлась, а
    // сошедшиеся до него числа оказались не теми.
    if (!reader.at(data.size())) {
        error = std::format("{}: {} bytes left over — the layout does not match",
                            path.string(), data.size());

        vehicles_.clear();
        peds_.clear();
        weapons_.clear();
        kits_.clear();
        return false;
    }

    loaded_ = true;
    return true;
}

const script::VehicleModelInfo* GameData::vehicle(std::uint32_t hash) const {
    const auto found = vehicles_.find(hash);
    return found == vehicles_.end() ? nullptr : &found->second;
}

const script::PedModelInfo* GameData::ped(std::uint32_t hash) const {
    const auto found = peds_.find(hash);
    return found == peds_.end() ? nullptr : &found->second;
}

const script::WeaponModelInfo* GameData::weapon(std::uint32_t hash) const {
    const auto found = weapons_.find(hash);
    return found == weapons_.end() ? nullptr : &found->second;
}

std::int32_t GameData::modsCount(std::uint32_t model, std::uint8_t slot) const {
    const script::VehicleModelInfo* const known = vehicle(model);

    if (known == nullptr) {
        return -1;
    }

    if (known->modKit == script::kNoModKit) {
        // Модель известна, набора у неё нет: тюнинга не бывает вовсе, и это
        // «ноль», а не «не знаю».
        return 0;
    }

    const auto kit = kits_.find(known->modKit);

    if (kit == kits_.end()) {
        return 0;
    }

    const auto found = kit->second.find(slot);
    return found == kit->second.end() ? 0 : static_cast<std::int32_t>(found->second);
}

} // namespace oxymp::server
