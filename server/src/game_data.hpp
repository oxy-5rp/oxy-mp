#pragma once

#include <oxymp/script/core.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace oxymp::server {

/// Справочники моделей: что сервер знает о машинах, людях, оружии и тюнинге.
///
/// Отвечают на четыре вопроса alt:V — `getVehicleModelInfoByHash`,
/// `getPedModelInfoByHash`, `getWeaponModelInfoByHash` и
/// `vehicle.getModsCount`, — и ни на один из них ответа нет ни в игре у
/// клиента, ни в реестрах сервера: это свойства самих моделей, лежащие в файлах
/// игры. alt:V держит их четырьмя двоичными файлами рядом с собой; мы —
/// одним, порождённым `tools/gamedata`.
///
/// **Файл необязателен.** Нет его — сервер поднимается как прежде, а четыре
/// вопроса отказывают вслух. Это не заглушка: отказ говорит, чего не хватает и
/// откуда его взять, а выдуманный ответ про модель увёл бы режим на часы.
class GameData {
public:
    /// Читает справочник. false — с объяснением в error.
    ///
    /// Отсутствие файла ошибкой не считается: он необязателен, и жаловаться на
    /// то, чего никто не обещал, значит пугать хозяина сервера впустую.
    /// Ошибка — это файл, который есть и не читается.
    [[nodiscard]] bool load(const std::filesystem::path& path, std::string& error);

    [[nodiscard]] bool loaded() const noexcept { return loaded_; }

    [[nodiscard]] const script::VehicleModelInfo* vehicle(std::uint32_t hash) const;
    [[nodiscard]] const script::PedModelInfo* ped(std::uint32_t hash) const;
    [[nodiscard]] const script::WeaponModelInfo* weapon(std::uint32_t hash) const;

    /// Сколько деталей есть у этой модели машины в этом месте тюнинга.
    ///
    /// Минус единица — про модель мы ничего не знаем; ноль — знаем и деталей в
    /// этом месте у неё нет. Разница не формальная: первое означает «спросите
    /// иначе», второе — «меню тюнинга здесь пустое», и показывать их одинаково
    /// нельзя.
    [[nodiscard]] std::int32_t modsCount(std::uint32_t model, std::uint8_t slot) const;

private:
    bool loaded_ = false;

    std::unordered_map<std::uint32_t, script::VehicleModelInfo> vehicles_;
    std::unordered_map<std::uint32_t, script::PedModelInfo> peds_;
    std::unordered_map<std::uint32_t, script::WeaponModelInfo> weapons_;

    /// Наборы тюнинга по номеру: место -> сколько деталей.
    ///
    /// Хранится числом, а не списком номеров деталей: спрашивают у них ровно
    /// одно — сколько. Понадобятся сами номера — они лежат в том же файле, и
    /// добавить их дешевле, чем нести их сейчас без надобности.
    std::unordered_map<std::uint16_t, std::unordered_map<std::uint8_t, std::uint8_t>> kits_;
};

} // namespace oxymp::server
