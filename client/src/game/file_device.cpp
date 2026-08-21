#include "file_device.hpp"

#include <spdlog/spdlog.h>

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace oxymp::client::game {
namespace {

/// Заголовок ресурса игры — того, чем являются модели, текстуры и всё, что
/// подгружается стримингом.
///
/// Простой файл от ресурса игра отличает по первым четырём байтам, а из
/// следующих двенадцати узнаёт, сколько под него отвести памяти обычной и
/// видео. Не ответив ей этого, мы получим отказ подгрузки: файл прочтётся,
/// но игра не будет знать, куда его класть.
struct ResourceHeader {
    std::uint32_t magic = 0;
    std::uint32_t version = 0;

    /// Раскладка страниц. Имена — те же, что у CitizenFX: `flag1` в
    /// `rage::ResourceFlags` это виртуальные страницы, `flag2` — физические.
    std::uint32_t virtualPages = 0;
    std::uint32_t physicalPages = 0;
};

/// «RSC7» младшим байтом вперёд.
constexpr std::uint32_t kResourceMagic = 0x37435352;

/// Что игра отдаёт вместо описателя, когда открыть не вышло.
///
/// Все единицы, а не ноль: ноль у неё — законный описатель. Ошибка здесь тихая
/// и дорогая — игра примет неудачу за успех и прочтёт мусор.
constexpr std::uint64_t kNoHandle = static_cast<std::uint64_t>(-1);

/// Приводит путь игры к виду, по которому его можно искать.
///
/// Игра пишет пути и так и эдак: `common:/data/…` и `COMMON:/DATA/…` для неё
/// один и тот же файл, а разделитель бывает и прямым, и обратным. Сравнивать их
/// как есть значит промахнуться на первом же обращении и не понять почему —
/// устройство отдаст «файла нет», и игра молча возьмёт свой.
std::string normalize(std::string_view path) {
    std::string result;
    result.reserve(path.size());

    for (const char c : path) {
        if (c == '\\') {
            result.push_back('/');
        } else {
            result.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(c))));
        }
    }

    return result;
}

} // namespace

/// Само устройство: объект с таблицей методов, которую понимает игра.
///
/// Раскладка таблицы задана игрой и перечислена ниже подряд, включая записи, о
/// которых известно только то, что они там есть. Пустые места оставлять нельзя:
/// сдвинув хоть одну запись, мы отдадим игре не ту функцию, и вылет случится не
/// на нашем коде, а где-то внутри неё.
struct FileDevice::Device {
    virtual ~Device() = default;

    // --- Чтение: то, ради чего всё и заведено ---------------------------------

    virtual std::uint64_t Open(const char* fileName, bool /*readOnly*/) {
        const std::filesystem::path* const source = find(fileName);
        if (source == nullptr) {
            return kNoHandle;
        }

        const HANDLE handle =
            ::CreateFileW(source->c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, nullptr);

        if (handle == INVALID_HANDLE_VALUE) {
            spdlog::warn("подменённый файл {} не открылся", fileName);
            return kNoHandle;
        }

        return reinterpret_cast<std::uint64_t>(handle);
    }

    /// Открытие для потоковой подгрузки.
    ///
    /// От обычного отличается тем, что игра запоминает начало файла внутри
    /// хранилища и потом читает по абсолютным смещениям. У нас хранилища нет —
    /// файл лежит файлом, — поэтому начало всегда ноль.
    virtual std::uint64_t OpenBulk(const char* fileName, std::uint64_t* base) {
        if (base != nullptr) {
            *base = 0;
        }

        return Open(fileName, true);
    }

    virtual std::uint64_t OpenBulkWrap(const char* fileName, std::uint64_t* base, void*) {
        return OpenBulk(fileName, base);
    }

    virtual std::uint64_t CreateLocal(const char*) { return kNoHandle; }
    virtual std::uint64_t Create(const char*) { return kNoHandle; }

    virtual std::uint32_t Read(std::uint64_t handle, void* buffer, std::uint32_t bytes) {
        DWORD read = 0;

        if (::ReadFile(toHandle(handle), buffer, bytes, &read, nullptr) == 0) {
            return 0;
        }

        return read;
    }

    /// Чтение по абсолютному смещению — то, чем игра подгружает модели.
    virtual std::uint32_t ReadBulk(std::uint64_t handle, std::uint64_t offset, void* buffer,
                                   std::uint32_t bytes) {
        OVERLAPPED where{};
        where.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFULL);
        where.OffsetHigh = static_cast<DWORD>(offset >> 32);

        DWORD read = 0;

        // Через OVERLAPPED, а не «встать и прочесть»: подгрузку игра ведёт
        // несколькими потоками по одному и тому же описателю, и общее положение
        // в файле они растащили бы друг у друга из-под ног.
        if (::ReadFile(toHandle(handle), buffer, bytes, &read, &where) == 0) {
            return 0;
        }

        return read;
    }

    virtual std::uint32_t WriteBulk(std::uint64_t, int, int, int, int) { return 0; }
    virtual std::uint32_t Write(std::uint64_t, void*, int) { return 0; }

    virtual std::uint32_t Seek(std::uint64_t handle, std::int32_t distance, std::uint32_t method) {
        return static_cast<std::uint32_t>(SeekLong(handle, distance, method));
    }

    virtual std::uint64_t SeekLong(std::uint64_t handle, std::int64_t distance,
                                   std::uint32_t method) {
        LARGE_INTEGER where{};
        where.QuadPart = distance;

        LARGE_INTEGER now{};

        if (::SetFilePointerEx(toHandle(handle), where, &now, method) == 0) {
            return kNoHandle;
        }

        return static_cast<std::uint64_t>(now.QuadPart);
    }

    virtual std::int32_t Close(std::uint64_t handle) {
        ::CloseHandle(toHandle(handle));
        return 0;
    }

    virtual std::int32_t CloseBulk(std::uint64_t handle) { return Close(handle); }

    virtual int GetFileLength(std::uint64_t handle) {
        return static_cast<int>(GetFileLengthUInt64(handle));
    }

    virtual std::uint64_t GetFileLengthUInt64(std::uint64_t handle) {
        LARGE_INTEGER size{};

        if (::GetFileSizeEx(toHandle(handle), &size) == 0) {
            return kNoHandle;
        }

        return static_cast<std::uint64_t>(size.QuadPart);
    }

    virtual int m_40(int) { return 0; }

    // --- Запись: у нас её нет вовсе -------------------------------------------
    //
    // Устройство только отдаёт: подменённые файлы приходят с сервера и меняться
    // игрой не должны. Отказ здесь — не заглушка, а верный ответ.

    virtual bool RemoveFile(const char*) { return false; }
    virtual int RenameFile(const char*, const char*) { return 0; }
    virtual int CreateDirectory(const char*) { return 0; }
    virtual int RemoveDirectory(const char*) { return 0; }
    virtual void m_xx() {}

    // --- Сведения о файле по пути ---------------------------------------------

    virtual std::uint64_t GetFileLengthLong(const char* fileName) {
        const std::filesystem::path* const source = find(fileName);
        if (source == nullptr) {
            return kNoHandle;
        }

        std::error_code failed;
        const auto size = std::filesystem::file_size(*source, failed);

        return failed ? kNoHandle : static_cast<std::uint64_t>(size);
    }

    virtual std::uint64_t GetFileTime(const char* fileName) {
        const std::filesystem::path* const source = find(fileName);
        if (source == nullptr) {
            return 0;
        }

        WIN32_FILE_ATTRIBUTE_DATA about{};
        if (::GetFileAttributesExW(source->c_str(), GetFileExInfoStandard, &about) == 0) {
            return 0;
        }

        return (static_cast<std::uint64_t>(about.ftLastWriteTime.dwHighDateTime) << 32) |
               about.ftLastWriteTime.dwLowDateTime;
    }

    virtual bool SetFileTime(const char*, FILETIME) { return false; }

    // --- Обход каталога -------------------------------------------------------
    //
    // Обходить у нас нечего: устройство вешается на путь одного файла, и всё,
    // что оно знает, — этот файл. Обход по нему пуст, и это правда, а не отказ.

    virtual std::uint64_t FindFirst(const char*, void*) { return kNoHandle; }
    virtual bool FindNext(std::uint64_t, void*) { return false; }
    virtual int FindClose(std::uint64_t) { return 0; }

    virtual Device* GetUnkDevice() { return this; }

    virtual void* m_xy(void*, int, void*) { return nullptr; }

    virtual bool Truncate(std::uint64_t) { return false; }

    virtual std::uint32_t GetFileAttributes(const char* fileName) {
        const std::filesystem::path* const source = find(fileName);
        if (source == nullptr) {
            return INVALID_FILE_ATTRIBUTES;
        }

        return ::GetFileAttributesW(source->c_str());
    }

    virtual bool m_xz() { return false; }

    virtual bool SetFileAttributes(const char*, std::uint32_t) { return false; }

    virtual int m_yx() { return 0; }

    /// Читает столько, сколько попросили, а не сколько отдалось за раз.
    ///
    /// Разница не косметическая: обычное чтение вправе вернуть меньше, и тот,
    /// кто зовёт это, на неполноту не рассчитывает.
    virtual bool ReadFull(std::uint64_t handle, void* buffer, std::uint32_t length) {
        auto* const bytes = static_cast<std::uint8_t*>(buffer);
        std::uint32_t filled = 0;

        while (filled < length) {
            const std::uint32_t got = Read(handle, bytes + filled, length - filled);
            if (got == 0) {
                return false;
            }

            filled += got;
        }

        return true;
    }

    virtual bool WriteFull(std::uint64_t, void*, std::uint32_t) { return false; }

    /// Версия ресурса игры и раскладка его страниц.
    ///
    /// Ноль означает «обычный файл» — и для описаний, настроек и разметки это
    /// правда. Модели же и текстуры ресурсы, и без этого ответа игра их не
    /// подгрузит: прочесть прочтёт, а сколько отвести памяти, знать не будет.
    ///
    /// Читается из первых шестнадцати байт самого файла: игра кладёт туда
    /// метку, версию и число страниц. Так же поступает и CitizenFX
    /// (`VFSRagePackfile::ExtensionCtl`).
    virtual std::int32_t GetResourceVersion(const char* fileName, void* flags) {
        auto* const out = static_cast<std::uint32_t*>(flags);

        if (out != nullptr) {
            out[0] = 0;
            out[1] = 0;
        }

        const std::filesystem::path* const source = find(fileName);
        if (source == nullptr) {
            return 0;
        }

        const HANDLE handle =
            ::CreateFileW(source->c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, nullptr);

        if (handle == INVALID_HANDLE_VALUE) {
            return 0;
        }

        ResourceHeader header{};
        DWORD read = 0;

        const bool got = ::ReadFile(handle, &header, sizeof(header), &read, nullptr) != 0 &&
                         read == sizeof(header);

        ::CloseHandle(handle);

        if (!got || header.magic != kResourceMagic) {
            return 0;
        }

        if (out != nullptr) {
            out[0] = header.virtualPages;
            out[1] = header.physicalPages;
        }

        return static_cast<std::int32_t>(header.version);
    }

    virtual std::int32_t m_yy() { return 0; }
    virtual std::int32_t m_yz(void*) { return 0; }
    virtual std::int32_t m_zx(void*) { return 0x40000000; }

    /// Хранилище ли это — то есть архив со многими файлами внутри.
    ///
    /// Нет: у нас файл лежит файлом. Ответить «да» значило бы пообещать игре
    /// оглавление, которого нет.
    virtual bool IsCollection() { return false; }

    virtual bool m_addedIn1290() { return false; }
    virtual Device* GetCollection() { return this; }
    virtual bool m_ax() { return false; }
    virtual std::int32_t GetCollectionId() { return 0; }

    virtual const char* GetName() { return "oxymp"; }

    // --- Своё хозяйство -------------------------------------------------------

    /// Что на что подменено. Ключ — путь игры, приведённый к общему виду.
    ///
    /// Под блокировкой, потому что читают его потоки подгрузки игры, а пишет наш
    /// — и пишет он не единожды при запуске: ресурсы приходят с сервера по мере
    /// подключения.
    mutable std::mutex mutex;
    std::unordered_map<std::string, std::filesystem::path> files;

    [[nodiscard]] const std::filesystem::path* find(const char* gamePath) const {
        if (gamePath == nullptr) {
            return nullptr;
        }

        const std::lock_guard guard{mutex};

        const auto found = files.find(normalize(gamePath));
        return found == files.end() ? nullptr : &found->second;
    }

    [[nodiscard]] static HANDLE toHandle(std::uint64_t handle) {
        return reinterpret_cast<HANDLE>(handle);
    }
};

std::unique_ptr<FileDevice> FileDevice::create(const EngineAddresses& addresses,
                                               std::string& error) {
    const auto mount = addresses.pointerTo<MountGlobal>("file_device_mount");
    if (mount == nullptr) {
        error = "адрес монтирования устройств не разрешился";
        return nullptr;
    }

    std::unique_ptr<FileDevice> owner{new FileDevice};
    owner->mount_ = mount;
    owner->device_ = new Device;

    return owner;
}

FileDevice::~FileDevice() {
    // Устройство не удаляется намеренно — см. объяснение при поле. Снять его из
    // таблицы монтирования игры нечем, а удалённое, оно оставило бы там
    // указатель в никуда.
}

void FileDevice::mount(std::string prefix) {
    const std::lock_guard guard{mutex_};

    const std::string wanted = normalize(prefix);

    if (std::ranges::find(prefixes_, wanted) != prefixes_.end() ||
        std::ranges::find(pending_, prefix) != pending_.end()) {
        return;
    }

    pending_.push_back(std::move(prefix));
}

std::size_t FileDevice::pump() {
    if (device_ == nullptr || mount_ == nullptr) {
        return 0;
    }

    std::vector<std::string> batch;

    {
        const std::lock_guard guard{mutex_};
        batch.swap(pending_);
    }

    std::size_t done = 0;

    for (const std::string& prefix : batch) {
        // allowRoot — true: без него игра не отдаёт устройству пути,
        // начинающиеся с корня своего пространства имён, а именно такие у неё
        // все.
        if (!mount_(prefix.c_str(), device_, true)) {
            spdlog::warn("игра не приняла устройство на приставке {}", prefix);
            continue;
        }

        {
            const std::lock_guard guard{mutex_};
            prefixes_.push_back(normalize(prefix));
        }

        ++done;

        // Строкой на приставку, а не на файл: файлов бывают тысячи, а приставок
        // единицы, и именно приставка — то, что игра либо приняла, либо нет.
        spdlog::info("своё устройство повешено на {}", prefix);
    }

    return done;
}

void FileDevice::serve(const std::string& gamePath, const std::filesystem::path& source) {
    if (device_ == nullptr) {
        return;
    }

    const std::lock_guard guard{device_->mutex};
    device_->files.insert_or_assign(normalize(gamePath), source);
}

std::size_t FileDevice::count() const noexcept {
    if (device_ == nullptr) {
        return 0;
    }

    const std::lock_guard guard{device_->mutex};
    return device_->files.size();
}

} // namespace oxymp::client::game
