#include "streaming_files.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace oxymp::client::game {

std::unique_ptr<StreamingFiles> StreamingFiles::create(const EngineAddresses& addresses,
                                                       std::string& error) {
    const auto entry = addresses.pointerTo<RegisterRawFile>("streaming_register_raw_file");
    if (entry == nullptr) {
        error = "адрес объявления файлов стриминга не разрешился";
        return nullptr;
    }

    std::unique_ptr<StreamingFiles> owner{new StreamingFiles};
    owner->register_ = entry;
    return owner;
}

void StreamingFiles::add(std::string path, std::string name) {
    const std::lock_guard guard{mutex_};

    if (std::ranges::find(declared_, name) != declared_.end()) {
        return;
    }

    for (const Wanted& each : pending_) {
        if (each.name == name) {
            return;
        }
    }

    pending_.push_back(Wanted{.path = std::move(path), .name = std::move(name)});
}

std::size_t StreamingFiles::pump() {
    if (register_ == nullptr) {
        return 0;
    }

    std::vector<Wanted> batch;

    {
        const std::lock_guard guard{mutex_};
        batch.swap(pending_);
    }

    std::size_t taken = 0;

    for (const Wanted& file : batch) {
        // Начальное значение обязано быть «нет номера»: игра пишет сюда только
        // при удаче, а при неудаче оставляет как было. Оставь мы здесь ноль — и
        // отказ выглядел бы как выданный нулевой номер, который у неё законен.
        std::uint32_t slot = kNoSlot;

        register_(&slot, file.path.c_str(), true, file.name.c_str(), false);

        if (slot == kNoSlot) {
            spdlog::warn("игра не взяла файл {} под именем {}", file.path, file.name);
            continue;
        }

        {
            const std::lock_guard guard{mutex_};
            declared_.push_back(file.name);
        }

        ++taken;

        spdlog::info("игре объявлен файл {} под именем {} (место {})", file.path, file.name,
                     slot);
    }

    return taken;
}

} // namespace oxymp::client::game
