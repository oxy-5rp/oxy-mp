#pragma once

#include "game_locator.hpp"
#include "game_store.hpp"

#include <string>

namespace oxymp::launcher {

/// Просит площадку запустить игру.
///
/// Запускает не игру, а площадку: у каждой из трёх свой способ, и все три взяты
/// у alt:V дословно, потому что придуманный самим здесь уже стоил рабочего
/// клиента.
///
///   Rockstar — `GTAVLauncher.exe -nobattleye` из каталога игры;
///   Steam    — `steam://run/271590/-nobattleye/`;
///   Epic     — `com.epicgames.launcher://apps/<id>?action=launch`.
///
/// `-nobattleye` — довод самой игры, а не наш выдуманный: им BattlEye не
/// поднимается. Подмена звена в лаунчере Rockstar остаётся вторым рубежом на
/// случай, если площадка всё же пойдёт через `GTA5_BE.exe`.
///
/// Процесса игры отсюда не достаётся никому: его создаёт площадка, у себя. Кто
/// запустил — тот и ждёт появления `GTA5.exe` по имени; так делает и alt:V
/// (`CGame::WaitForStartup`), и это единственный способ, работающий на всех трёх
/// площадках сразу.
[[nodiscard]] bool startGameThroughPlatform(const GameLocation& location, std::string& error);

} // namespace oxymp::launcher
