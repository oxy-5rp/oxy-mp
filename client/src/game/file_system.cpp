#include "file_system.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace oxymp::client::game {
namespace {

/// Места нужных нам методов в таблице методов устройства.
///
/// Порядок взят из объявления `rage::fiDevice` в исходниках CitizenFX
/// (`code/components/rage-device-five/include/fiDevice.h`): там класс объявлен
/// так, чтобы его таблица методов совпадала с игровой до последней записи.
///
/// Нулевая запись — разрушитель: MSVC отводит ему одно место, и всё остальное
/// сдвинуто на единицу. Ошибка здесь означает вызов не той функции игры с не
/// теми доводами, то есть вылет без объяснений, — поэтому номера выписаны
/// поимённо, а не считаются на месте.
constexpr std::size_t kOpen = 1;
constexpr std::size_t kRead = 6;
constexpr std::size_t kClose = 12;
constexpr std::size_t kFileLengthByPath = 22;
constexpr std::size_t kFindFirst = 25;
constexpr std::size_t kFindNext = 26;
constexpr std::size_t kFindClose = 27;

/// Запись обхода каталога — `rage::fiFindData`.
///
/// Раскладка взята из объявления в исходниках CitizenFX. Имя первым и длиной в
/// 256 байт: игра пишет его туда без оглядки на нас, и укоротить поле значило бы
/// получить запись поверх своего стека.
struct FindData {
    char fileName[256];
    std::uint64_t fileSize;
    std::uint32_t writeTimeLow;
    std::uint32_t writeTimeHigh;
    std::uint32_t attributes;
};

/// Признак каталога у Windows; игра отдаёт те же признаки.
constexpr std::uint32_t kDirectory = 0x10;

/// Насколько глубоко заходить в обходе.
///
/// Предел от кольца, а не от жадности: устройство вправе отдать «каталог» с тем
/// же именем, что и родитель, и обход пошёл бы по кругу. У самых глубоких чужих
/// архивов вложенность не доходит и до половины этого.
constexpr int kMaxDepth = 8;

/// Достаёт из таблицы методов запись по номеру и приводит её к нужному виду.
template <typename Signature>
[[nodiscard]] Signature methodAt(void* object, std::size_t index) {
    // Таблица лежит по нулевому смещению объекта — так устроен всякий объект с
    // виртуальными методами у MSVC.
    void** const table = *static_cast<void***>(object);
    return reinterpret_cast<Signature>(table[index]);
}

using OpenMethod = std::uint64_t (*)(void* device, const char* path, bool readOnly);
using ReadMethod = std::uint32_t (*)(void* device, std::uint64_t handle, void* buffer,
                                     std::uint32_t bytes);
using CloseMethod = std::int32_t (*)(void* device, std::uint64_t handle);
using LengthMethod = std::uint64_t (*)(void* device, const char* path);
using FindFirstMethod = std::uint64_t (*)(void* device, const char* path, FindData* found);
using FindNextMethod = bool (*)(void* device, std::uint64_t handle, FindData* found);
using FindCloseMethod = int (*)(void* device, std::uint64_t handle);

/// Что игра отдаёт вместо описателя, когда открыть не вышло.
constexpr std::uint64_t kNoHandle = static_cast<std::uint64_t>(-1);

/// Предел на размер читаемого файла.
///
/// Читаем мы описания и настройки — они измеряются килобытами. Архив целиком
/// сюда попасть не должен: он не поместится в памяти, а если и поместится, то
/// незачем.
constexpr std::uint64_t kMaxFileSize = 64ull * 1024ull * 1024ull;

} // namespace

FileSystem::FileSystem(const EngineAddresses& addresses) noexcept
    : lookup_{addresses.pointerTo<DeviceLookup>("file_device_lookup")},
      mount_{addresses.pointerTo<void*>("file_device_mount")} {}

bool FileSystem::ready() const noexcept {
    return lookup_ != nullptr && mount_ != nullptr;
}

std::int64_t FileSystem::sizeOf(const char* path) const {
    if (!ready() || path == nullptr) {
        return -1;
    }

    void* const device = lookup_(path, true);
    if (device == nullptr) {
        return -1;
    }

    const auto length = methodAt<LengthMethod>(device, kFileLengthByPath)(device, path);

    // Игра отвечает всеми единицами, когда файла нет: у неё это то же значение,
    // что и «описатель недействителен».
    return length == kNoHandle ? -1 : static_cast<std::int64_t>(length);
}

std::string FileSystem::read(const char* path) const {
    if (!ready() || path == nullptr) {
        return {};
    }

    void* const device = lookup_(path, true);
    if (device == nullptr) {
        spdlog::debug("устройства для {} у игры нет", path);
        return {};
    }

    const std::uint64_t length = methodAt<LengthMethod>(device, kFileLengthByPath)(device, path);
    if (length == kNoHandle || length == 0 || length > kMaxFileSize) {
        return {};
    }

    const std::uint64_t handle = methodAt<OpenMethod>(device, kOpen)(device, path, true);
    if (handle == kNoHandle || handle == 0) {
        spdlog::debug("файл {} не открылся", path);
        return {};
    }

    std::string contents(static_cast<std::size_t>(length), '\0');

    // Читаем в цикле: устройство вправе отдать меньше запрошенного за раз — это
    // не ошибка, а обычный ход дела для архива, из которого данные ещё
    // распаковываются.
    std::size_t filled = 0;

    while (filled < contents.size()) {
        const auto asked = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(contents.size() - filled, 1024ull * 1024ull));

        const std::uint32_t got =
            methodAt<ReadMethod>(device, kRead)(device, handle, contents.data() + filled, asked);

        if (got == 0) {
            break;
        }

        filled += got;
    }

    methodAt<CloseMethod>(device, kClose)(device, handle);

    contents.resize(filled);
    return contents;
}

std::vector<std::string> FileSystem::filesUnder(const std::string& directory) const {
    std::vector<std::string> files;

    if (!ready()) {
        return files;
    }

    // Обход в ширину своим списком, а не рекурсией: у чужих архивов бывает и
    // тысяча каталогов, а глубина стека внутри кадра игры не наша.
    struct Step {
        std::string path;
        int depth;
    };

    std::vector<Step> ahead;

    // Косая черта на конце обязательна, и это наблюдение, а не догадка: у
    // CitizenFX обход тоже начинается с `cfx:/addons/`, а не с `cfx:/addons`.
    std::string root = directory;
    if (root.empty() || root.back() != '/') {
        root += '/';
    }

    ahead.push_back(Step{.path = std::move(root), .depth = 0});

    while (!ahead.empty()) {
        const Step here = ahead.back();
        ahead.pop_back();

        void* const device = lookup_(here.path.c_str(), true);
        if (device == nullptr) {
            continue;
        }

        FindData found{};

        std::uint64_t handle =
            methodAt<FindFirstMethod>(device, kFindFirst)(device, here.path.c_str(), &found);

        // Некоторые устройства ждут путь каталога без косой черты на конце.
        if (handle == kNoHandle && here.path.size() > 1 && here.path.back() == '/') {
            const std::string bare = here.path.substr(0, here.path.size() - 1);
            handle = methodAt<FindFirstMethod>(device, kFindFirst)(device, bare.c_str(), &found);
        }

        if (handle == kNoHandle) {
            spdlog::debug("the game refused to walk {}", here.path);
            continue;
        }

        do {
            // Имя приходит из чужой памяти: если игра не оставила завершающего
            // нуля, строка ушла бы читать дальше поля.
            found.fileName[sizeof(found.fileName) - 1] = '\0';

            const std::string_view name{found.fileName};

            if (name.empty() || name == "." || name == "..") {
                continue;
            }

            // Путь каталога сам кончается косой чертой, поэтому вторую не
            // добавляем: игра примет и такое, но в журнале это выглядело бы
            // ошибкой там, где её нет.
            std::string full = here.path;
            if (full.empty() || full.back() != '/') {
                full += '/';
            }
            full += name;

            if ((found.attributes & kDirectory) != 0) {
                if (here.depth < kMaxDepth) {
                    full += '/';
                    ahead.push_back(Step{.path = std::move(full), .depth = here.depth + 1});
                }

                continue;
            }

            files.push_back(std::move(full));
        } while (methodAt<FindNextMethod>(device, kFindNext)(device, handle, &found));

        methodAt<FindCloseMethod>(device, kFindClose)(device, handle);
    }

    return files;
}

} // namespace oxymp::client::game
