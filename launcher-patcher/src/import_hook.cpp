#include "import_hook.hpp"

#include <format>

#include <windows.h>

namespace oxymp::patcher {
namespace {

/// Заголовок PE по базе загруженного модуля. nullptr, если по адресу лежит не он.
const IMAGE_NT_HEADERS* headersOf(const void* module) {
    const auto* const base = static_cast<const std::uint8_t*>(module);
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);

    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return nullptr;
    }

    const auto* const headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);

    return headers->Signature == IMAGE_NT_SIGNATURE ? headers : nullptr;
}

/// Записывает значение в ячейку таблицы импорта.
///
/// Таблица лежит в странице только для чтения, поэтому запись обрамляется
/// сменой прав. Права возвращаются на место сразу же: оставить чужую таблицу
/// доступной для записи — значит оставить после себя дыру.
bool writeSlot(void** slot, void* value) {
    DWORD previous = 0;
    if (::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous) == 0) {
        return false;
    }

    *slot = value;

    DWORD restored = 0;
    ::VirtualProtect(slot, sizeof(void*), previous, &restored);

    return true;
}

/// Ищет ячейку таблицы импорта по имени функции.
///
/// Имя берётся из таблицы имён (OriginalFirstThunk), а подменяется ячейка в
/// таблице адресов (FirstThunk): первая описывает, что просили, вторая хранит
/// то, что подставил загрузчик.
void** findByName(const std::uint8_t* base, const IMAGE_IMPORT_DESCRIPTOR* descriptor,
                  std::string_view function) {
    for (; descriptor->Name != 0; ++descriptor) {
        // Без таблицы имён сопоставлять не с чем: у такого модуля имена
        // потеряны, и остаётся только поиск по адресу.
        if (descriptor->OriginalFirstThunk == 0) {
            continue;
        }

        const auto* names =
            reinterpret_cast<const IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
        auto* addresses = reinterpret_cast<IMAGE_THUNK_DATA*>(
            const_cast<std::uint8_t*>(base) + descriptor->FirstThunk);

        for (; names->u1.AddressOfData != 0; ++names, ++addresses) {
            // Импорт по порядковому номеру имени не несёт вовсе.
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                continue;
            }

            const auto* const imported =
                reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);

            if (function == imported->Name) {
                return reinterpret_cast<void**>(&addresses->u1.Function);
            }
        }
    }

    return nullptr;
}

/// Ищет ячейку таблицы импорта по адресу, который в ней лежит.
///
/// Запасной путь для случая, когда имён в таблице нет: модуль мог быть собран
/// со связанным импортом. Сравнивается с тем, что отдаёт сама система, поэтому
/// работает и через переходники api-ms-win-core-*.
void** findByAddress(const std::uint8_t* base, const IMAGE_IMPORT_DESCRIPTOR* descriptor,
                     const void* resolved) {
    if (resolved == nullptr) {
        return nullptr;
    }

    for (; descriptor->Name != 0; ++descriptor) {
        auto* addresses = reinterpret_cast<IMAGE_THUNK_DATA*>(
            const_cast<std::uint8_t*>(base) + descriptor->FirstThunk);

        for (; addresses->u1.Function != 0; ++addresses) {
            if (reinterpret_cast<const void*>(addresses->u1.Function) == resolved) {
                return reinterpret_cast<void**>(&addresses->u1.Function);
            }
        }
    }

    return nullptr;
}

} // namespace

std::unique_ptr<ImportHook> ImportHook::install(void* module, std::string_view function,
                                                void* replacement, std::string& error) {
    const IMAGE_NT_HEADERS* const headers = headersOf(module);
    if (headers == nullptr) {
        error = "there is no PE header at the module address";
        return nullptr;
    }

    const IMAGE_DATA_DIRECTORY& imports =
        headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress == 0) {
        error = "the module has no import table";
        return nullptr;
    }

    auto* const base = static_cast<std::uint8_t*>(module);
    const auto* const descriptor =
        reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);

    void** slot = findByName(base, descriptor, function);

    if (slot == nullptr) {
        const std::string name{function};
        const HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
        const void* const resolved =
            kernel32 == nullptr
                ? nullptr
                : reinterpret_cast<const void*>(::GetProcAddress(kernel32, name.c_str()));

        slot = findByAddress(base, descriptor, resolved);
    }

    if (slot == nullptr) {
        error = std::format("{} is not in the import table", function);
        return nullptr;
    }

    std::unique_ptr<ImportHook> hook{new ImportHook};
    hook->slot_ = slot;
    hook->original_ = *slot;

    if (!writeSlot(slot, replacement)) {
        error = std::format("could not write to the import table: Windows error {}",
                            ::GetLastError());
        return nullptr;
    }

    return hook;
}

ImportHook::~ImportHook() {
    if (slot_ == nullptr) {
        return;
    }

    // Вернуть исходный адрес обязательно: оставленный перехват — это переход в
    // наш модуль из чужого кода, и выгрузись модуль раньше, лаунчер ушёл бы
    // исполнять освобождённую память.
    writeSlot(slot_, original_);
}

} // namespace oxymp::patcher
