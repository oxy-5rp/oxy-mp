#include "packfiles.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <format>
#include <new>

namespace oxymp::client::game {
namespace {

/// Сколько памяти отвести под объект архива.
///
/// Настоящий размер `rage::fiPackfile` — около шести сотен байт: указатель на
/// таблицу методов и поле-заполнитель, размеченное в исходниках CitizenFX как
/// `368 + (0x650 - 0x590)`. Берём с большим запасом и обнуляем: цена — четыре
/// килобайта на архив, а ошибка в другую сторону — переходник игры, пишущий за
/// концом нашей памяти.
constexpr std::size_t kObjectSize = 0x1000;

/// Метка архива — 32-разрядное число 0x52504637.
///
/// На диске оно лежит младшим байтом вперёд, то есть байтами `37 46 50 52`, и
/// просмотрщик показывает их как «7FPR», а вовсе не «RPF7». Разница не
/// косметическая: файл, начинающийся буквами `RPF7`, — не архив, а чья-то
/// подделка, и первым же на такой и попались.
constexpr std::uint32_t kArchiveMagic = 0x52504637U;

/// Длина заголовка архива: метка, число записей, длина имён, метка шифрования.
constexpr std::size_t kArchiveHeaderLength = 16;

/// Метка зашифрованного оглавления — та, что стоит в настоящих архивах.
///
/// Какая именно это схема — обычная AES или та, где ключ выбирается по имени и
/// размеру, — здесь не утверждается: проверить это нечем, а гадать про архивы
/// уже стоило вылетов. Установлено о ней ровно два факта, и оба наблюдением:
/// она стоит на архивах, которые раздают люди, и игра на ней падает.
///
/// Прежде эта же метка звалась здесь `kEncryptedAes` и **принималась**, а рядом
/// лежала вторая, `0x0FEFFFFE`, — на единицу меньше и не встречающаяся нигде.
/// Обе были выведены рассуждением, а не сняты с файла.
///
/// **Принимать её нельзя, и это выяснено вылетом, а не рассуждением.** Игра
/// такой архив открывает, но оглавление у неё остаётся зашифрованным (см.
/// `docs/altv-next.md`, раздел про архивы): условие, при котором она пускает
/// ключ в ход для чужого архива, до сих пор не найдено. Дальше она читает из
/// шифротекста число записей и смещения — то есть из мусора — и пишет за концом
/// отведённого:
///
///     0xC0000005 в GTA5.exe+0x1362ed3, запись, вызвано из oxymp-client.dll
///
/// Тот же адрес, что и у битого заголовка, и та же причина: игра идёт по
/// оглавлению, которого не поняла. Поймано на живом архиве машины 24 августа
/// 2026 — игра падала через четыре миллисекунды после разбора заголовка.
///
/// Отсюда правило: пока не найдено, чем заставить игру расшифровать оглавление
/// чужого архива, такой архив ей не отдаётся вовсе. Отказ в журнале несравнимо
/// лучше вылета: у отказа видна причина, а вылет уносит игру целиком.
constexpr std::uint32_t kEncryptedTable = 0x0FEFFFFFU;

/// «OPEN» буквами — метка незашифрованного оглавления.
///
/// Её игра не просто не принимает — она **зависает** внутри открытия, и картинка
/// встаёт намертво. Поймано сторожем кадра: «the game frame has been stuck for
/// 5 s at opening game archives».
constexpr std::uint32_t kOpenTable = 0x4E45504FU;

/// Сколько записей в архиве считать возможным.
///
/// Предел от испорченного файла, а не от жадности. У самых больших архивов
/// игры записей десятки тысяч; миллион — заведомо больше всего, что бывает, и
/// заведомо меньше того, что читается из мусора.
constexpr std::uint32_t kMaxArchiveEntries = 1'000'000;

/// Похож ли файл на настоящий архив игры.
///
/// **Проверка написана кровью.** Игра, получив архив с испорченным заголовком,
/// не отказывается — она падает внутри себя: читает из мусора число записей,
/// отводит под оглавление сколько скажут и пишет за концом. Проверено живым
/// вылетом (0xC0000005 в GTA5.exe+0x1362ed3) на файле, у которого метка была
/// настоящая, а всё за ней — случайные байты.
///
/// Отсюда правило: **всё, что приходит с сервера, проверяется до передачи
/// игре**. Хозяин сервера не обязан быть злым, чтобы уронить всех игроков, —
/// довольно выложить недокачанный файл.
[[nodiscard]] bool looksLikeArchive(const std::filesystem::path& path, std::string& why) {
    std::error_code failed;
    const auto size = std::filesystem::file_size(path, failed);

    if (failed || size < kArchiveHeaderLength) {
        why = "the file is shorter than a header";
        return false;
    }

    std::FILE* file = nullptr;
    if (::fopen_s(&file, path.string().c_str(), "rb") != 0 || file == nullptr) {
        why = "the file did not open";
        return false;
    }

    std::uint8_t head[kArchiveHeaderLength]{};
    const bool read = std::fread(head, 1, sizeof(head), file) == sizeof(head);
    std::fclose(file);

    if (!read) {
        why = "the header did not read";
        return false;
    }

    const auto little = [&head](std::size_t at) {
        return static_cast<std::uint32_t>(head[at]) |
               (static_cast<std::uint32_t>(head[at + 1]) << 8) |
               (static_cast<std::uint32_t>(head[at + 2]) << 16) |
               (static_cast<std::uint32_t>(head[at + 3]) << 24);
    };

    if (little(0) != kArchiveMagic) {
        why = "no RPF7 mark";
        return false;
    }

    const std::uint32_t entries = little(4);
    const std::uint32_t namesLength = little(8);
    const std::uint32_t encryption = little(12);

    // Проверки формы идут первыми и остаются живыми, хотя вердикт ниже сегодня
    // и так отрицательный. Они написаны по вылетам, и терять их нельзя: решится
    // вопрос с оглавлением — они снова окажутся единственным, что стоит между
    // недокачанным файлом на сервере и падением у всех, кто зашёл.
    if (entries == 0 || entries > kMaxArchiveEntries) {
        why = std::format("impossible number of entries: {}", entries);
        return false;
    }

    // Оглавление обязано помещаться в файл: у испорченного заголовка оно
    // «занимает» больше, чем весь архив, и это видно сразу.
    const std::uint64_t needed = static_cast<std::uint64_t>(kArchiveHeaderLength) +
                                 static_cast<std::uint64_t>(entries) * 16 + namesLength;

    if (needed > size) {
        why = std::format("the table of contents does not fit: {} bytes needed, file is {}", needed,
                          size);
        return false;
    }

    // А дальше — вердикт по оглавлению, и сегодня он отрицательный для всякой
    // встречающейся метки. Это состояние честное, а не затычка: обе, что бывают
    // на самом деле, игру убивают — `OPEN` зависанием, «ключ по имени и размеру»
    // вылетом. Отдавать ей архив, зная это, значит ронять игрока сознательно.
    //
    // Сама дорога при этом остаётся рабочей и проверенной до последнего шага:
    // устройство отдаёт файл, игра его открывает, заполняет объект и вешает на
    // приставку. Не хватает ровно одного звена — того, чем её заставить
    // расшифровать чужое оглавление, — и оно названо в `docs/altv-next.md`.
    // Найдётся оно — здесь появится метка, которая проходит, и всё остальное
    // заработает без единой правки.
    if (encryption == kOpenTable) {
        why = "the table of contents is unencrypted (OPEN), and the game hangs on those";
        return false;
    }

    if (encryption == kEncryptedTable) {
        why = "the game will not decrypt this table of contents for an archive of ours, "
              "and walking it undecrypted crashes the game";
        return false;
    }

    why = std::format("unknown table-of-contents encryption mark: {:#010x}", encryption);
    return false;
}

/// Где в объекте архива лежит признак «пускать к корню».
///
/// Снято с кода самой игры: `fiPackfile::Mount` читает `[this+0x114]` и передаёт
/// прочитанное третьим доводом в MountGlobal, а третий довод там — allowRoot.
/// Своего пути выставить его нет: доводом Mount его не принимает.
constexpr std::size_t kAllowRootFlag = 0x114;

/// Вид архива у OpenPackfile. Тройка — обычный `.rpf`, лежащий файлом.
///
/// Число взято из обвязки CitizenFX: у них тот же вызов с тем же видом стоит в
/// упрощённой перегрузке «открыть архив с диска».
constexpr int kPlainArchive = 3;

} // namespace

std::unique_ptr<Packfiles> Packfiles::create(const EngineAddresses& addresses,
                                             std::string& error) {
    const auto construct = addresses.pointerTo<Construct>("packfile_construct");
    const auto open = addresses.pointerTo<Open>("packfile_open");
    const auto mountAt = addresses.pointerTo<MountAt>("packfile_mount");

    if (construct == nullptr || open == nullptr || mountAt == nullptr) {
        error = "the game archive addresses did not resolve";
        return nullptr;
    }

    std::unique_ptr<Packfiles> owner{new Packfiles};
    owner->construct_ = construct;
    owner->open_ = open;
    owner->mountAt_ = mountAt;

    return owner;
}

void Packfiles::mount(std::filesystem::path archive, std::string request, std::string prefix) {
    const std::lock_guard guard{mutex_};

    if (std::ranges::find(mounted_, prefix) != mounted_.end()) {
        return;
    }

    for (const Wanted& each : pending_) {
        if (each.prefix == prefix) {
            return;
        }
    }

    pending_.push_back(Wanted{.archive = std::move(archive),
                              .request = std::move(request),
                              .prefix = std::move(prefix)});
}

std::vector<std::string> Packfiles::pump(const FileSystem& files) {
    std::vector<std::string> opened;

    if (open_ == nullptr) {
        return opened;
    }

    std::vector<Wanted> batch;

    {
        const std::lock_guard guard{mutex_};
        batch.swap(pending_);
    }

    for (const Wanted& each : batch) {
        // Заголовок проверяется до всего остального: битый архив игра не
        // отвергает, а роняет собой всю игру.
        if (std::string why; !looksLikeArchive(each.archive, why)) {
            spdlog::warn("{} is not a game archive and was not opened: {}",
                         each.archive.filename().string(), why);
            continue;
        }

        // Игра обязана видеть файл по тому пути, который мы ей назовём.
        //
        // Проверка не лишняя и написана по вылету: не найдя файла, игра не
        // отказывается открывать архив — она читает его заголовок из памяти,
        // которую никто не заполнил, получает оттуда число записей и пишет за
        // концом отведённого. Падение при этом случается внутри неё, а не у
        // нас, и по адресу падения причина не видна никак.
        const std::int64_t seen = files.sizeOf(each.request.c_str());

        if (seen <= 0) {
            spdlog::warn("the game does not see archive {} at {}",
                         each.archive.filename().string(), each.request);
            continue;
        }

        // Память под объект — своя и навсегда. Освободить её нельзя: игра
        // держит ссылку на устройство в своей таблице монтирования до конца
        // процесса, а снять монтирование ей нечем. Освобождённый объект оставил
        // бы ей указатель в никуда, и первое же чтение из архива уронило бы её.
        void* const object = ::operator new(kObjectSize, std::nothrow);
        if (object == nullptr) {
            spdlog::warn("archive {} was not opened: out of memory", each.archive.string());
            continue;
        }

        std::fill_n(static_cast<std::uint8_t*>(object), kObjectSize, std::uint8_t{0});

        construct_(object);

        const std::string path = each.request;

        spdlog::debug("opening archive {}", path);

        if (!open_(object, path.c_str(), true, kPlainArchive, 0)) {
            // Молчать нельзя: архив, который не открылся, проявится не ошибкой,
            // а пустым местом в мире — и искать причину будут долго.
            spdlog::warn("the game did not open archive {}", path);
            continue;
        }

        // Признак «пускать к корню» ставится прямо в поле объекта: Mount
        // берёт его оттуда, а не из довода. Смещение снято с кода самой игры —
        // `mov r8b, [rcx+114h]` перед вызовом MountGlobal, а третий довод у
        // MountGlobal это как раз allowRoot.
        //
        // Без него игра не отдаёт устройству пути, начинающиеся с корня её
        // пространства имён, — а именно такие у неё все.
        static_cast<std::uint8_t*>(object)[kAllowRootFlag] = 1;

        mountAt_(object, each.prefix.c_str());

        {
            const std::lock_guard guard{mutex_};
            mounted_.push_back(each.prefix);
        }

        opened.push_back(each.prefix);
        spdlog::debug("archive {} mounted at {}", path, each.prefix);
    }

    return opened;
}

std::size_t Packfiles::count() const noexcept {
    const std::lock_guard guard{mutex_};
    return mounted_.size();
}

} // namespace oxymp::client::game
