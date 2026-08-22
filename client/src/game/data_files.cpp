#include "data_files.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iterator>

namespace oxymp::client::game {
namespace {

/// Хеш имени вида описания.
///
/// Тот же Jenkins one-at-a-time, что и `shared::joaat`, но **без приведения к
/// нижнему регистру**, и это не мелочь. Обычный хеш игры регистр снижает, а
/// таблица видов хранит хеши имён как они написаны — заглавными. Проверено
/// счётом: первая запись таблицы это `RPF_FILE`, и сходится она только без
/// снижения регистра.
[[nodiscard]] std::uint32_t hashTypeName(std::string_view name) noexcept {
    std::uint32_t hash = 0;

    for (const char symbol : name) {
        hash += static_cast<unsigned char>(symbol);
        hash += hash << 10U;
        hash ^= hash >> 6U;
    }

    hash += hash << 3U;
    hash ^= hash >> 11U;
    hash += hash << 15U;

    return hash;
}

/// Какое имя файла каким видом описания считается.
///
/// Перечнем, а не разбором содержимого: вид у игры называется именем, а имя
/// файла с ним связано договорённостью, которой столько же лет, сколько модам
/// для GTA. Заводить сюда всё подряд незачем — здесь только то, из чего состоит
/// машина.
///
/// Порядок в перечне не случаен и соблюдается при загрузке: поведение раньше
/// самой машины, потому что машина ссылается на него по имени.
struct KnownFile {
    std::string_view fileName;
    std::string_view type;
};

constexpr std::array kKnownFiles{
    KnownFile{"handling.meta", "HANDLING_FILE"},
    KnownFile{"vehicles.meta", "VEHICLE_METADATA_FILE"},
    KnownFile{"carcols.meta", "CARCOLS_FILE"},
    KnownFile{"carvariations.meta", "VEHICLE_VARIATION_FILE"},
    KnownFile{"vehiclelayouts.meta", "VEHICLE_LAYOUTS_FILE"},
};

/// В каком порядке описания попадают к игре.
///
/// Порядок здесь не украшение: описание, на которое ссылаются, обязано попасть
/// к игре раньше ссылающегося. Архетипы — раньше всего, на них ссылается любая
/// поставленная в мир вещь; поведение машины — раньше самой машины, потому что
/// машина зовёт его по имени.
///
/// По виду, а не по имени файла, и это не мелочь: вид называет сервер, и имя
/// файла у карты не говорит о нём ничего. Всё, чего в перечне нет, грузится
/// после перечисленного, сохраняя порядок, в котором пришло.
constexpr std::array kLoadOrder{
    std::string_view{"DLC_ITYP_REQUEST"},
    std::string_view{"HANDLING_FILE"},
    std::string_view{"VEHICLE_METADATA_FILE"},
    std::string_view{"CARCOLS_FILE"},
    std::string_view{"VEHICLE_VARIATION_FILE"},
    std::string_view{"VEHICLE_LAYOUTS_FILE"},
};

/// Сколько времени за кадр отдаётся загрузке описаний.
///
/// Причина та же, что и у объявления файлов, только острее: описание игра не
/// просто открывает, а разбирает. Список архетипов чужой карты — это сотни
/// таких разборов, и все в одном кадре складывались в секунды неподвижной
/// картинки.
///
/// Четыре миллисекунды — четверть кадра при шестидесяти в секунду.
constexpr auto kFrameBudget = std::chrono::milliseconds{4};

/// Сколько записей таблицы видов согласны прочесть.
///
/// Предел от испорченного адреса, а не от жадности: таблица кончается особой
/// записью, и не найдя её, обход ушёл бы гулять по чужой памяти до первого
/// недоступного адреса. У игры видов около двух сотен.
constexpr std::size_t kMaxTypes = 4096;

/// Место LoadDataFile в таблице методов загрузчика.
///
/// Первое после разрушителя, и это снято с кода игры: в месте вызова стоит
/// `mov rax,[rcx]; call [rax+8]` — то есть вторая запись таблицы.
constexpr std::size_t kLoadDataFile = 1;

} // namespace

std::unique_ptr<DataFiles> DataFiles::create(const EngineAddresses& addresses,
                                             std::string& error) {
    const auto mounters = addresses.pointerTo<void**>("data_file_mounters");
    const auto types = addresses.pointerTo<const TypeEntry*>("data_file_types");

    if (mounters == nullptr || types == nullptr) {
        error = "адреса загрузчика описаний не разрешились";
        return nullptr;
    }

    std::unique_ptr<DataFiles> owner{new DataFiles};
    owner->mounters_ = mounters;
    owner->types_ = types;

    return owner;
}

std::int32_t DataFiles::typeOf(std::string_view name) const {
    if (types_ == nullptr) {
        return -1;
    }

    const std::uint32_t wanted = hashTypeName(name);

    for (std::size_t i = 0; i < kMaxTypes; ++i) {
        const TypeEntry& entry = types_[i];

        // Таблица кончается записью с нулевым хешем и всеми единицами в номере.
        if (entry.hash == 0 && entry.index == 0xFFFFFFFFU) {
            break;
        }

        if (entry.hash == wanted) {
            return static_cast<std::int32_t>(entry.index);
        }
    }

    return -1;
}

void DataFiles::add(std::string gamePath, std::string_view fileName,
                    std::string_view declared) {
    const std::string lowered = [fileName] {
        std::string result;
        result.reserve(fileName.size());

        for (const char symbol : fileName) {
            const auto letter = static_cast<unsigned char>(symbol);
            result.push_back(letter >= 'A' && letter <= 'Z'
                                 ? static_cast<char>(letter + ('a' - 'A'))
                                 : symbol);
        }

        return result;
    }();

    const auto known = std::ranges::find(kKnownFiles, lowered, &KnownFile::fileName);

    // Названный сервером вид главнее угаданного, и спорить тут не о чем: имя
    // вида знает тот, кто ресурс собирал, а мы знаем четыре имени файлов.
    const std::string_view wanted =
        !declared.empty() ? declared
                          : (known == kKnownFiles.end() ? std::string_view{} : known->type);

    if (wanted.empty()) {
        // Не всякое описание рядом с машиной нам знакомо, и молчать об этом
        // нельзя: пропущенное описание проявится не ошибкой, а машиной без
        // цвета или без звука — и искать причину будут долго.
        spdlog::debug("data file {} skipped: the server did not name its type", fileName);
        return;
    }

    const std::int32_t type = typeOf(wanted);

    if (type < 0) {
        spdlog::warn("data file {} skipped: this game has no type \"{}\"", fileName, wanted);
        return;
    }

    if (gamePath.size() >= sizeof(Entry::name)) {
        spdlog::warn("data file {} skipped: the path is longer than the game accepts", gamePath);
        return;
    }

    const std::lock_guard guard{mutex_};

    // Отмечаем и спрашиваем одним движением: вставка отвечает, было ли о пути
    // известно раньше.
    if (!known_.insert(gamePath).second) {
        return;
    }

    const auto place = std::ranges::find(kLoadOrder, wanted);

    pending_.push_back(Wanted{
        .path = std::move(gamePath),
        .type = type,
        .order = static_cast<std::size_t>(std::distance(kLoadOrder.begin(), place)),
    });
}

std::size_t DataFiles::pump() {
    if (mounters_ == nullptr) {
        return 0;
    }

    std::vector<Wanted> batch;

    {
        const std::lock_guard guard{mutex_};
        batch.swap(pending_);
    }

    if (batch.empty()) {
        return 0;
    }

    // Порядок — тот же, что в kLoadOrder: то, на что ссылаются, раньше
    // ссылающегося. Сортировать по номеру вида нельзя — его назначает игра, и
    // он ничего не говорит о том, что от чего зависит.
    std::ranges::stable_sort(batch, {}, &Wanted::order);

    std::size_t loaded = 0;
    std::size_t done = 0;

    const auto started = std::chrono::steady_clock::now();

    for (const Wanted& file : batch) {
        ++done;

        void* const mounter = mounters_[file.type];

        if (mounter == nullptr) {
            spdlog::warn("data file {} not loaded: the game has no mounter for type {}", file.path,
                         file.type);
            continue;
        }

        Entry entry{};
        std::memcpy(entry.name, file.path.c_str(), file.path.size());
        entry.type = file.type;

        // Таблица методов лежит по нулевому смещению объекта — так устроен
        // всякий объект с виртуальными методами у MSVC.
        void** const table = *static_cast<void***>(mounter);

        using LoadDataFile = bool (*)(void* self, Entry* entry);
        const auto load = reinterpret_cast<LoadDataFile>(table[kLoadDataFile]);

        if (!load(mounter, &entry)) {
            spdlog::warn("the game refused to load data file {}", file.path);
            continue;
        }

        ++loaded;
        spdlog::debug("игре загружено описание {} (вид {})", file.path, file.type);

        // Хотя бы одно описание за кадр загружается всегда, даже если срок
        // вышел ещё до начала: иначе очередь не тронулась бы с места.
        if (std::chrono::steady_clock::now() - started >= kFrameBudget) {
            break;
        }
    }

    // Недоделанное возвращается в начало очереди. Порядок при этом сохраняется
    // сам собой: батч отсортирован, а остаток берётся с его конца.
    if (done < batch.size()) {
        const std::lock_guard guard{mutex_};

        pending_.insert(pending_.begin(), std::make_move_iterator(batch.begin() + done),
                        std::make_move_iterator(batch.end()));
    }

    return loaded;
}

} // namespace oxymp::client::game
