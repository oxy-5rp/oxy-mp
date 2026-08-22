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

/// Метки шифрования оглавления, которые игра умеет читать.
///
/// Их две: обычная (AES) и та, у которой ключ выбирается по имени и размеру
/// файла. Третья, `OPEN`, означает незашифрованное оглавление — и вот её игра не
/// принимает: путей без расшифровки у неё нет вовсе.
///
/// **Отсеивать такие обязательно.** Игра на них не отказывается — она
/// **зависает** внутри открытия, и картинка встаёт намертво. Поймано сторожем
/// кадра на живом архиве: «the game frame has been stuck for 5 s at opening game
/// archives». Без этой проверки один такой файл на сервере вешал бы всех, кто
/// зашёл.
constexpr std::uint32_t kEncryptedAes = 0x0FEFFFFFU;
constexpr std::uint32_t kEncryptedNg = 0x0FEFFFFEU;

/// «OPEN» буквами — метка незашифрованного оглавления.
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
        why = "файл короче заголовка";
        return false;
    }

    std::FILE* file = nullptr;
    if (::fopen_s(&file, path.string().c_str(), "rb") != 0 || file == nullptr) {
        why = "файл не открылся";
        return false;
    }

    std::uint8_t head[kArchiveHeaderLength]{};
    const bool read = std::fread(head, 1, sizeof(head), file) == sizeof(head);
    std::fclose(file);

    if (!read) {
        why = "заголовок не прочёлся";
        return false;
    }

    const auto little = [&head](std::size_t at) {
        return static_cast<std::uint32_t>(head[at]) |
               (static_cast<std::uint32_t>(head[at + 1]) << 8) |
               (static_cast<std::uint32_t>(head[at + 2]) << 16) |
               (static_cast<std::uint32_t>(head[at + 3]) << 24);
    };

    if (little(0) != kArchiveMagic) {
        why = "нет метки RPF7";
        return false;
    }

    const std::uint32_t entries = little(4);
    const std::uint32_t namesLength = little(8);
    const std::uint32_t encryption = little(12);

    if (encryption != kEncryptedAes && encryption != kEncryptedNg) {
        why = encryption == kOpenTable
                  ? std::string{"оглавление незашифровано (OPEN), а такие игра не открывает"}
                  : std::format("неизвестная метка шифрования оглавления: {:#010x}", encryption);
        return false;
    }

    if (entries == 0 || entries > kMaxArchiveEntries) {
        why = std::format("невозможное число записей: {}", entries);
        return false;
    }

    // Оглавление обязано помещаться в файл: у испорченного заголовка оно
    // «занимает» больше, чем весь архив, и это видно сразу.
    const std::uint64_t needed = static_cast<std::uint64_t>(kArchiveHeaderLength) +
                                 static_cast<std::uint64_t>(entries) * 16 + namesLength;

    if (needed > size) {
        why = std::format("оглавление не помещается: нужно {} байт при размере {}", needed, size);
        return false;
    }

    return true;
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
        error = "адреса работы с архивами игры не разрешились";
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
        // Признак «пускать к корню» ставится прямо в поле объекта: Mount
        // берёт его оттуда, а не из довода. Смещение снято с кода самой игры —
        // `mov r8b, [rcx+114h]` перед вызовом MountGlobal, а третий довод у
        // MountGlobal это как раз allowRoot.
        //
        // Без него игра не отдаёт устройству пути, начинающиеся с корня её
        // пространства имён, — а именно такие у неё все.
        {
            // ВРЕМЕННО: смотрим, заполнила ли игра объект вообще.
            const auto* const bytes = static_cast<const std::uint8_t*>(object);
            std::string head;
            for (std::size_t i = 0; i < 0x60; ++i) {
                head += std::format("{:02x}", bytes[i]);
                if (i % 8 == 7) {
                    head += ' ';
                }
            }
            spdlog::warn("ARC object after open: {}", head);

            // Смотрим, что лежит по указателям из объекта: где-то там имена
            // записей, уже расшифрованные игрой.
            for (const std::size_t at : {std::size_t{0x10}, std::size_t{0x20},
                                         std::size_t{0x50}}) {
                const auto* const where =
                    *reinterpret_cast<const std::uint8_t* const*>(bytes + at);

                if (where == nullptr) {
                    continue;
                }

                std::string text;
                for (std::size_t i = 0; i < 160; ++i) {
                    const std::uint8_t c = where[i];
                    text += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.';
                }

                spdlog::warn("ARC at +{:#x}: {}", at, text);
            }
        }

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
