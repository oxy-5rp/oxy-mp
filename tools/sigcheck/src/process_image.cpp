#include "process_image.hpp"

#include "pe_headers.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <memory>
#include <type_traits>

#include <windows.h>

#include <tlhelp32.h>

namespace oxymp::sigcheck {
namespace {

struct HandleCloser {
    void operator()(HANDLE handle) const noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle);
        }
    }
};

using UniqueHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleCloser>;

/// Заголовки PE занимают первую страницу модуля — этого хватает и на таблицу секций.
constexpr std::size_t kHeaderReadSize = 0x1000;

std::string lastErrorText() {
    return std::format("код ошибки Windows {}", ::GetLastError());
}

/// Узкое представление имени модуля — только для отчёта.
///
/// Имена исполняемых файлов игры состоят из ASCII, но сужение всё равно делается
/// явно: неявное молча портит всё, что за пределами таблицы.
std::string narrow(const std::wstring& text) {
    std::string result;
    result.reserve(text.size());

    for (const wchar_t character : text) {
        result.push_back(character < 0x80 ? static_cast<char>(character) : '?');
    }

    return result;
}

std::optional<DWORD> findProcessId(const std::wstring& processName) {
    const UniqueHandle snapshot{::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (snapshot.get() == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (!::Process32FirstW(snapshot.get(), &entry)) {
        return std::nullopt;
    }

    do {
        if (::_wcsicmp(entry.szExeFile, processName.c_str()) == 0) {
            return entry.th32ProcessID;
        }
    } while (::Process32NextW(snapshot.get(), &entry));

    return std::nullopt;
}

/// Смещение поля ImageBaseAddress в PEB 64-разрядного процесса.
constexpr std::size_t kPebImageBaseOffset = 0x10;

/// PROCESS_BASIC_INFORMATION в том виде, в каком её описывает winternl.h.
///
/// Структуру нужно передавать целиком: получив меньший размер, ядро отвечает
/// STATUS_INFO_LENGTH_MISMATCH и ничего не заполняет. Поля, которые нам не нужны,
/// оставлены безымянными — их назначение не документировано и может меняться.
struct ProcessBasicInformationLayout {
    PVOID reserved1;
    PVOID pebBaseAddress;
    PVOID reserved2[2];
    ULONG_PTR uniqueProcessId;
    PVOID reserved3;
};

static_assert(sizeof(ProcessBasicInformationLayout) == 48,
              "PROCESS_BASIC_INFORMATION на x64 занимает 48 байт");

using NtQueryInformationProcessFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

/// База главного модуля процесса.
///
/// Читается из PEB, а не из списка модулей: у процессов с защитой от
/// вмешательства перечисление модулей через Toolhelp регулярно отказывает,
/// тогда как PEB доступен тем же дескриптором, что и остальная память.
std::optional<std::uint64_t> findMainModuleBase(HANDLE process, std::string& error) {
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        error = "не удалось получить ntdll.dll";
        return std::nullopt;
    }

    const auto queryInformation = reinterpret_cast<NtQueryInformationProcessFn>(
        reinterpret_cast<void*>(::GetProcAddress(ntdll, "NtQueryInformationProcess")));
    if (queryInformation == nullptr) {
        error = "в ntdll.dll нет NtQueryInformationProcess";
        return std::nullopt;
    }

    ProcessBasicInformationLayout information{};
    ULONG written = 0;

    constexpr ULONG kProcessBasicInformation = 0;
    const LONG status = queryInformation(process, kProcessBasicInformation, &information,
                                         sizeof(information), &written);
    if (status < 0 || information.pebBaseAddress == nullptr) {
        error = std::format("не удалось прочитать сведения о процессе (статус {:#x})",
                            static_cast<std::uint32_t>(status));
        return std::nullopt;
    }

    const auto pebAddress = reinterpret_cast<std::uint64_t>(information.pebBaseAddress);

    std::uint64_t moduleBase = 0;
    SIZE_T read = 0;
    if (!::ReadProcessMemory(process, reinterpret_cast<LPCVOID>(pebAddress + kPebImageBaseOffset),
                             &moduleBase, sizeof(moduleBase), &read) ||
        read != sizeof(moduleBase) || moduleBase == 0) {
        error = std::format("не удалось прочитать базу модуля из PEB: {}", lastErrorText());
        return std::nullopt;
    }

    return moduleBase;
}

/// Читает область памяти процесса, возвращая число байт, прочитать которые не вышло.
///
/// Крупные блоки читаются одним вызовом, но если блок задет недоступной страницей,
/// ReadProcessMemory отказывает целиком — поэтому такой блок дочитывается
/// постранично, а недоступные страницы обнуляются.
std::uint64_t readRegion(HANDLE process, std::uint64_t address, std::size_t size,
                         std::uint8_t* destination) {
    constexpr std::size_t kChunkSize = 64 * 1024;
    constexpr std::size_t kPageSize = 4096;

    std::uint64_t unreadable = 0;
    std::size_t done = 0;

    while (done < size) {
        const std::size_t chunk = std::min(kChunkSize, size - done);
        SIZE_T read = 0;

        const bool ok = ::ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address + done),
                                            destination + done, chunk, &read) != 0;
        if (ok && read == chunk) {
            done += chunk;
            continue;
        }

        for (std::size_t offset = 0; offset < chunk; offset += kPageSize) {
            const std::size_t page = std::min(kPageSize, chunk - offset);
            SIZE_T pageRead = 0;

            const bool pageOk =
                ::ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address + done + offset),
                                    destination + done + offset, page, &pageRead) != 0;
            if (!pageOk || pageRead != page) {
                std::memset(destination + done + offset, 0, page);
                unreadable += page;
            }
        }

        done += chunk;
    }

    return unreadable;
}

} // namespace

std::unique_ptr<ProcessImage> ProcessImage::attach(const std::wstring& processName,
                                                   std::string& error) {
    const auto processId = findProcessId(processName);
    if (!processId) {
        error = "процесс не найден — игра не запущена?";
        return nullptr;
    }

    const UniqueHandle process{
        ::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, *processId)};
    if (!process) {
        if (::GetLastError() == ERROR_ACCESS_DENIED) {
            error = "доступ к процессу запрещён — попробуйте запустить от имени администратора";
        } else {
            error = std::format("не удалось открыть процесс: {}", lastErrorText());
        }
        return nullptr;
    }

    const auto moduleBase = findMainModuleBase(process.get(), error);
    if (!moduleBase) {
        return nullptr;
    }

    std::vector<std::uint8_t> headerBytes(kHeaderReadSize);
    if (readRegion(process.get(), *moduleBase, headerBytes.size(), headerBytes.data()) != 0) {
        error = "не удалось прочитать заголовки модуля";
        return nullptr;
    }

    auto headers = peheaders::parse(memscan::ByteView{headerBytes.data(), headerBytes.size()}, error);
    if (!headers) {
        return nullptr;
    }

    std::unique_ptr<ProcessImage> image{new ProcessImage};
    image->baseAddress_ = *moduleBase;
    image->sections_ = std::move(headers->sections);
    image->data_.reserve(image->sections_.size());

    for (const Section& section : image->sections_) {
        std::vector<std::uint8_t> buffer(section.virtualSize);

        if (!buffer.empty()) {
            image->unreadableBytes_ += readRegion(process.get(), *moduleBase + section.rva,
                                                  buffer.size(), buffer.data());
        }

        image->data_.push_back(std::move(buffer));
    }

    image->origin_ = std::format("процесс {} (pid {}), модуль по адресу {:#x}", narrow(processName),
                                 *processId, *moduleBase);

    return image;
}

std::size_t ProcessImage::indexOf(const Section& section) const noexcept {
    for (std::size_t i = 0; i < sections_.size(); ++i) {
        if (sections_[i].rva == section.rva) {
            return i;
        }
    }
    return sections_.size();
}

memscan::ByteView ProcessImage::sectionData(const Section& section) const noexcept {
    const std::size_t index = indexOf(section);
    if (index >= data_.size()) {
        return {};
    }

    return memscan::ByteView{data_[index].data(), data_[index].size()};
}

const std::uint8_t* ProcessImage::rvaToPointer(std::uint64_t rva, std::size_t needed) const noexcept {
    for (std::size_t i = 0; i < sections_.size(); ++i) {
        const Section& section = sections_[i];
        if (rva < section.rva) {
            continue;
        }

        const std::uint64_t offsetInSection = rva - section.rva;
        const std::size_t available = data_[i].size();

        if (offsetInSection >= available || available - offsetInSection < needed) {
            continue;
        }

        return data_[i].data() + offsetInSection;
    }

    return nullptr;
}

} // namespace oxymp::sigcheck
