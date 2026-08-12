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

/// Заводит второе имя для того же файла.
///
/// Именно второе имя, а не копию: содержимое на диске одно, и второй раз оно не
/// занимает ничего. Работает в пределах тома — здесь это соблюдено тем, что
/// зеркало лежит внутри самой игры.
bool linkFile(const std::filesystem::path& from, const std::filesystem::path& to) {
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

/// Проходит каталог игры и достраивает зеркало недостающими ссылками.
///
/// Возвращает число сделанных ссылок, либо пусто при ошибке.
std::optional<std::size_t> mirrorTree(const std::filesystem::path& source,
                                      const std::filesystem::path& target,
                                      const std::filesystem::path& skipDirectory,
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
        // Само зеркало обходится стороной: оно лежит внутри игры, и без этого
        // проход ушёл бы отражать сам себя.
        if (entry.path() == skipDirectory) {
            continue;
        }

        // GTA5.exe не трогаем никогда, и это не мелочь, а вся суть.
        //
        // Ниже стоит правило «содержимое разошлось — переставить ссылку». Для
        // игрового мира оно верное, для GTA5.exe — разрушительное: обновлённая
        // игра выглядит как раз разошедшейся, и правило заменило бы закреплённую
        // копию на ту самую новую, от которой мы закреплялись.
        if (entry.path() == skipFile) {
            continue;
        }

        const std::filesystem::path destination = target / entry.path().filename();

        if (entry.is_directory(ec)) {
            const auto nested = mirrorTree(entry.path(), destination, skipDirectory, skipFile,
                                           error, needsAdministrator);
            if (!nested) {
                return std::nullopt;
            }

            linked += *nested;
            continue;
        }

        if (!entry.is_regular_file(ec)) {
            continue;
        }

        // Уже связанное не трогаем: зеркало готовится при каждом запуске, и
        // переделывать сто тысяч файлов ради десятка изменившихся незачем.
        if (std::filesystem::exists(destination, ec)) {
            if (sameFile(entry.path(), destination)) {
                continue;
            }

            // Разошлось — значит игру обновили. Старое имя отпускаем: пусть
            // указывает на новое содержимое. Закреплён у нас GTA5.exe, а не
            // весь мир.
            std::filesystem::remove(destination, ec);
        }

        if (!linkFile(entry.path(), destination)) {
            const DWORD failure = ::GetLastError();

            // Отказ в доступе здесь означает ровно одно, и об этом стоит сказать
            // прямо: игра лежит там, куда обычному пользователю писать не дают.
            // Человек, увидевший «код 5», не поймёт ничего.
            if (failure == ERROR_ACCESS_DENIED) {
                needsAdministrator = true;

                error = std::format(
                    "Нужны права администратора: игра стоит в {}, а завести второе имя "
                    "для чужого файла может только тот, кто вправе его менять.",
                    source.string());
            } else {
                error = std::format("не удалось связать {}: код {}", destination.string(),
                                    failure);
            }

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
    const bool besidePlayer = volumeOf(existingAncestor(preferred)) == gameVolume;

    const std::filesystem::path mirror =
        besidePlayer ? preferred : game.directory / kMirrorInsideGame;

    if (!besidePlayer) {
        spdlog::info("oxyMP и игра на разных дисках — копия ляжет к игре: {}", mirror.string());
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

        spdlog::info("GTA5.exe закреплён: {}", pinned.string());
    } else {
        report.gameUpdated = !sameFile(game.executable, pinned);
    }

    const auto linked = mirrorTree(game.directory, mirror, mirror, game.executable, error,
                                   needsAdministrator);
    if (!linked) {
        return std::nullopt;
    }

    report.linked = *linked;

    if (report.gameUpdated) {
        spdlog::warn("игра обновилась, но запускаем закреплённую копию: {}", pinned.string());
    }

    spdlog::info("зеркало игры готово: {}, новых ссылок {}", mirror.string(), report.linked);

    return report;
}

} // namespace oxymp::launcher
