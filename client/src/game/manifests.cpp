#include "manifests.hpp"

#include "file_device.hpp"

#include <spdlog/spdlog.h>

#include <memory>
#include <utility>

namespace oxymp::client::game {

std::unique_ptr<Manifests> Manifests::create(const EngineAddresses& addresses,
                                             std::string& error) {
    const auto load = addresses.pointerTo<LoadManifest>("manifest_load");
    const auto init = addresses.pointerTo<ChunkStep>("manifest_chunk_init");
    const auto apply = addresses.pointerTo<ChunkStep>("manifest_chunk_apply");
    const auto clear = addresses.pointerTo<ChunkStep>("manifest_chunk_clear");
    void* const chunk = addresses.pointerTo<void*>("manifest_chunk");

    if (load == nullptr || init == nullptr || apply == nullptr || clear == nullptr ||
        chunk == nullptr) {
        error = "the manifest reader addresses did not resolve";
        return nullptr;
    }

    std::unique_ptr<Manifests> manifests{new Manifests};

    manifests->load_ = load;
    manifests->init_ = init;
    manifests->apply_ = apply;
    manifests->clear_ = clear;
    manifests->chunk_ = chunk;

    spdlog::debug("разбор описей готов: {:#x}", reinterpret_cast<std::uintptr_t>(load));

    return manifests;
}

void Manifests::add(std::filesystem::path file, std::string tag) {
    const std::lock_guard guard{mutex_};
    pending_.emplace_back(std::move(file), std::move(tag));
}

std::size_t Manifests::pump(FileDevice& device) {
    std::vector<std::pair<std::filesystem::path, std::string>> batch;

    {
        const std::lock_guard guard{mutex_};
        batch.swap(pending_);
    }

    if (batch.empty()) {
        return 0;
    }

    // Устройство вешается на приставку описей при первой надобности и остаётся
    // висеть: снять монтирование нечем — обратного вызова у него нет. Опасности
    // в этом меньше, чем кажется: отвечаем мы только пока стоит подмена «на
    // всё», а её мы снимаем сразу после разбора. Всё остальное время устройство
    // под этой приставкой не отвечает ничем, и игра ищет своё дополнение сама.
    if (!mounted_) {
        device.mount(kMountPoint);

        // Монтирование откладывается до кадра игры, а мы уже в нём — значит
        // просьбу надо тут же и исполнить, иначе первая опись не найдётся.
        (void)device.pump();

        mounted_ = true;
    }

    std::size_t done = 0;

    for (const auto& [path, tag] : batch) {
        // Опись отдаётся на любой запрос под приставкой: путь, по которому игра
        // за ней пойдёт, строится внутри её кода и нам неизвестен.
        device.answerAllWith(kMountPoint, path);

        // Порядок из четырёх шагов, и переставлять их нельзя: применение читает
        // то, что записал разбор, а освобождение стирает и то и другое.
        init_(chunk_);
        load_(chunk_, reinterpret_cast<void*>(1), tag.c_str());
        apply_(chunk_);
        clear_(chunk_);

        ++done;

        spdlog::debug("опись {} разобрана ({})", tag, path.string());
    }

    // Подмена снимается сразу: под этой приставкой игра ищет свои дополнения, и
    // отвечать ей нашей описью на них нельзя.
    device.answerAllWith(kMountPoint, {});

    spdlog::info("Map manifests read: {}", done);

    return done;
}

} // namespace oxymp::client::game
