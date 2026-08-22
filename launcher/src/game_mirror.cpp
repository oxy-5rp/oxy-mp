#include "game_mirror.hpp"

#include <spdlog/spdlog.h>

#include <cwchar>
#include <format>
#include <system_error>

#include <windows.h>

namespace oxymp::launcher {
namespace {

constexpr const wchar_t* kExecutableName = L"GTA5.exe";

/// Куда класть копию, когда oxyMP и игра оказались на разных дисках.
///
/// Внутрь каталога игры. Место так себе — при переустановке игры копия пропадёт
/// вместе с ней, — но оно на нужном диске по определению, а больше ни на что
/// рассчитывать нельзя: где у человека есть свободное место и право писать,
/// заранее не известно.
constexpr const wchar_t* kMirrorInsideGame = L"oxymp-backup";

/// На каком томе лежит путь.
///
/// Спрашивается у Windows, а не разбирается по букве диска: букв может не быть
/// вовсе — том умеет быть подключённым в каталог.
std::wstring volumeOf(const std::filesystem::path& path) {
    std::wstring volume(MAX_PATH, L'\0');

    if (::GetVolumePathNameW(path.c_str(), volume.data(),
                             static_cast<DWORD>(volume.size())) == 0) {
        return {};
    }

    volume.resize(std::wcslen(volume.c_str()));
    return volume;
}

/// Ближайший существующий предок пути.
///
/// Нужен потому, что тома спрашивают у того, что есть: у несуществующего пути
/// Windows тома не назовёт, а зеркало как раз ещё не создано.
std::filesystem::path existingAncestor(std::filesystem::path path) {
    std::error_code ec;

    while (!path.empty() && !std::filesystem::exists(path, ec)) {
        const std::filesystem::path parent = path.parent_path();
        if (parent == path) {
            break;
        }

        path = parent;
    }

    return path;
}

/// Заводит ссылку на чужой файл или каталог, не занимая места.
///
/// Способов два, и выбор между ними — не вкусовщина, а свойства файловой
/// системы.
///
/// Жёсткая ссылка — это второе имя того же файла. Лучше всего, что можно
/// придумать: места не занимает вовсе и переживает обновление игры, потому что
/// обновление подменяет файл целиком, а наше имя продолжает указывать на прежнее
/// содержимое. Но она не пересекает границу диска.
///
/// Символьная ссылка — это указание «смотри туда». Границу диска пересекает, и
/// потому именно ею зеркало живёт рядом с oxymp.exe, где бы тот ни лежал. Плата:
/// она не закрепляет содержимое — обновив игру, увидим обновлённое. Для данных
/// это терпимо, а закрепляем мы то, что и нужно закрепить: GTA5.exe копией.
///
/// Каталоги умеет только символьная ссылка, и это к лучшему: вместо обхода ста
/// тысяч файлов получается четыре ссылки.
bool linkEntry(const std::filesystem::path& from, const std::filesystem::path& to,
               bool sameVolume, bool directory, bool& needsAdministrator) {
    if (directory || !sameVolume) {
        DWORD flags = directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;

        // Windows 10 с включённым режимом разработчика позволяет заводить
        // символьные ссылки без прав администратора. Пробуем сперва так: лишний
        // запрос прав — то, чего человек не ждёт и пугается.
        if (::CreateSymbolicLinkW(to.c_str(), from.c_str(),
                                  flags | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0) {
            return true;
        }

        if (::CreateSymbolicLinkW(to.c_str(), from.c_str(), flags) != 0) {
            return true;
        }

        if (const DWORD failure = ::GetLastError();
            failure == ERROR_PRIVILEGE_NOT_HELD || failure == ERROR_ACCESS_DENIED) {
            needsAdministrator = true;
        }

        return false;
    }

    if (::CreateHardLinkW(to.c_str(), from.c_str(), nullptr) != 0) {
        return true;
    }

    // Файловая система без жёстких ссылок — не повод сдаваться: обычная копия
    // тоже годится, просто занимает место. Так бывает на FAT32 и на сетевых
    // дисках.
    //
    // Список причин узкий намеренно. Соблазн подставлять копию на любую неудачу
    // велик и обманчив: в игре есть файлы по нескольку гигабайт, и «на всякий
    // случай» здесь означало бы молча занять десятки гигабайт вместо того, чтобы
    // сказать, что не вышло.
    const DWORD failure = ::GetLastError();
    if (failure == ERROR_NOT_SUPPORTED || failure == ERROR_INVALID_FUNCTION) {
        std::error_code ec;
        std::filesystem::copy_file(from, to, ec);
        return !ec;
    }

    return false;
}

/// Одинаковы ли два файла по размеру и времени правки.
///
/// Побайтного сравнения здесь нет намеренно: файлы бывают по несколько
/// гигабайт, а вопрос стоит не «одинаковы ли они», а «не подменили ли один из
/// них». На подмену размер и время отзываются, а стоят они ничего.
bool sameFile(const std::filesystem::path& left, const std::filesystem::path& right) {
    std::error_code ec;

    const auto leftSize = std::filesystem::file_size(left, ec);
    if (ec) {
        return false;
    }

    const auto rightSize = std::filesystem::file_size(right, ec);
    if (ec || leftSize != rightSize) {
        return false;
    }

    const auto leftTime = std::filesystem::last_write_time(left, ec);
    if (ec) {
        return false;
    }

    const auto rightTime = std::filesystem::last_write_time(right, ec);
    return !ec && leftTime == rightTime;
}

/// Достраивает зеркало недостающими ссылками.
///
/// Обхода вглубь здесь нет намеренно. Каталоги игры связываются целиком, одной
/// ссылкой на каждый: `update` и `x64` — это восемьдесят гигабайт и десятки
/// тысяч файлов, и отражать их по одному значило бы делать за минуты то, что
/// делается мгновенно.
///
/// Возвращает число сделанных ссылок, либо пусто при ошибке.
std::optional<std::size_t> mirrorTree(const std::filesystem::path& source,
                                      const std::filesystem::path& target, bool sameVolume,
                                      const std::filesystem::path& skipFile, std::string& error,
                                      bool& needsAdministrator) {
    std::error_code ec;
    std::filesystem::create_directories(target, ec);

    if (ec) {
        error = std::format("не удалось создать каталог {}: {}", target.string(), ec.message());
        return std::nullopt;
    }

    std::size_t linked = 0;

    for (const auto& entry : std::filesystem::directory_iterator{source, ec}) {
        // GTA5.exe не трогаем никогда, и это не мелочь, а вся суть: он у нас
        // настоящей копией, и ссылка на его место всё бы отменила.
        if (entry.path() == skipFile) {
            continue;
        }

        const std::filesystem::path destination = target / entry.path().filename();
        const bool directory = entry.is_directory(ec);

        if (!directory && !entry.is_regular_file(ec)) {
            continue;
        }

        // Уже связанное не трогаем: зеркало готовится при каждом запуске, а
        // ссылки не портятся сами по себе.
        if (std::filesystem::exists(destination, ec) ||
            std::filesystem::is_symlink(destination, ec)) {
            continue;
        }

        if (!linkEntry(entry.path(), destination, sameVolume, directory, needsAdministrator)) {
            error = needsAdministrator
                        ? std::string{"Нужны права администратора: без них Windows не даёт "
                                      "заводить ссылки на чужие файлы.\n"
                                      "Включённый «режим разработчика» в настройках Windows "
                                      "снимает это требование навсегда."}
                        : std::format("не удалось связать {}: код {}", destination.string(),
                                      ::GetLastError());

            return std::nullopt;
        }

        ++linked;
    }

    if (ec) {
        error = std::format("не удалось прочитать каталог {}: {}", source.string(), ec.message());
        return std::nullopt;
    }

    return linked;
}

} // namespace

std::optional<GameMirror::Report> GameMirror::prepare(const GameLocation& game,
                                                      const std::filesystem::path& preferred,
                                                      std::string& error,
                                                      bool& needsAdministrator) {
    needsAdministrator = false;

    const std::wstring gameVolume = volumeOf(game.directory);

    // Рядом с oxymp.exe, если это тот же диск, что и у игры. Иначе — внутрь
    // самой игры.
    //
    // Второй путь хуже: копия оказывается не там, где человек её ищет, и
    // пропадает при переустановке игры. Но он единственный работающий, когда
    // диски разные, а разные они у многих: игры ставят на большой диск, а
    // программы на системный.
    //
    // Отдельной платы за это нет. Права администратора нужны в обоих случаях —
    // не ради места, а ради самих ссылок: игра лежит там, куда обычному
    // пользователю писать не дают.
    // Всегда рядом с oxymp.exe, на каком бы диске он ни лежал.
    //
    // Раньше при разных дисках копия уезжала к игре, и это было решение не той
    // задачи: человек ставит oxyMP туда, куда хочет, и переносить его ради нас
    // он не обязан. Разные диски снимаются выбором вида ссылки — см. linkEntry.
    const std::filesystem::path mirror = preferred;
    const bool sameVolume = volumeOf(existingAncestor(mirror)) == gameVolume;

    if (!sameVolume) {
        spdlog::info("oxyMP and the game are on different drives: linking with symlinks");
    }

    const std::filesystem::path pinned = mirror / kExecutableName;

    std::error_code ec;
    std::filesystem::create_directories(mirror, ec);

    if (ec) {
        // Каталог внутри игры обычным пользователем не создаётся — это тот же
        // случай нехватки прав, просто замеченный раньше, чем дошло до ссылок.
        needsAdministrator = ec.value() == ERROR_ACCESS_DENIED;

        error = std::format("не удалось создать каталог {}: {}", mirror.string(), ec.message());
        return std::nullopt;
    }

    Report report;
    report.location.directory = mirror;
    report.location.executable = pinned;
    report.location.version = game.version;

    // Площадка у копии та же: закрепляя игру, мы переносим и её переходник к
    // Steam, и права она спросит там же, где спросила бы исходная.
    report.location.store = game.store;

    // Настоящая копия, а не ссылка, и это самое существенное место здесь.
    //
    // Ссылка на GTA5.exe отменила бы всю затею: обновление подменило бы файл, а
    // мы бы честно запустили подменённый — тот самый, под который у нас нет
    // сигнатур. Копия закрепляет байты, которые мы умеем читать.
    if (!std::filesystem::exists(pinned, ec)) {
        if (::CopyFileW(game.executable.c_str(), pinned.c_str(), TRUE) == 0) {
            const DWORD failure = ::GetLastError();
            needsAdministrator = failure == ERROR_ACCESS_DENIED;

            error = std::format("не удалось скопировать GTA5.exe: код {}", failure);
            return std::nullopt;
        }

        spdlog::info("GTA5.exe pinned: {}", pinned.string());
    } else {
        report.gameUpdated = !sameFile(game.executable, pinned);
    }

    const auto linked = mirrorTree(game.directory, mirror, sameVolume, game.executable, error,
                                   needsAdministrator);
    if (!linked) {
        return std::nullopt;
    }

    report.linked = *linked;

    if (report.gameUpdated) {
        spdlog::warn("the game updated, but the pinned copy is being started: {}", pinned.string());
    }

    spdlog::info("Game mirror ready: {}, {} new links", mirror.string(), report.linked);

    return report;
}

} // namespace oxymp::launcher
