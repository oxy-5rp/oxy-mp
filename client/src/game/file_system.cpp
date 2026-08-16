#include "file_system.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

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

} // namespace oxymp::client::game
