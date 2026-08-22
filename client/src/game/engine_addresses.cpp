#include "engine_addresses.hpp"

#include <oxymp/gamesig/catalog.hpp>
#include <oxymp/gamesig/resolver.hpp>

#include <spdlog/spdlog.h>

#include <format>

namespace oxymp::client::game {
namespace {

/// Идентификатор сигнатуры, по которой сверяется сборка игры.
constexpr std::string_view kBuildDateId = "build_date_string";

/// Ограничение на длину строки с датой сборки. Настоящая заметно короче.
constexpr std::size_t kBuildDateMaxLength = 32;

} // namespace

std::unique_ptr<EngineAddresses> EngineAddresses::resolveCatalog(const gamesig::ImageSource& image,
                                                                 std::string& error,
                                                                 Failure& failure) {
    // По умолчанию неудача считается временной: код игры разворачивается не
    // сразу, и первые попытки не находят ничего по совершенно нормальной
    // причине.
    failure = Failure::NotReady;

    std::unique_ptr<EngineAddresses> addresses{new EngineAddresses};

    // Дата сборки идёт первой: если игра обновилась, остальные сигнатуры могут
    // совпасть не там, где нужно, и при этом остаться формально однозначными.
    const gamesig::Signature* buildDate = gamesig::find(kBuildDateId);
    if (buildDate == nullptr) {
        error = std::format("в каталоге нет сигнатуры \"{}\"", kBuildDateId);
        return nullptr;
    }

    const gamesig::Resolved resolvedDate = gamesig::resolve(image, *buildDate);
    if (!resolvedDate.ok()) {
        error = std::format("не удалось найти дату сборки игры: {}",
                            gamesig::describe(resolvedDate.status));
        return nullptr;
    }

    addresses->buildDate_ =
        gamesig::readCString(image, resolvedDate.targetRva, kBuildDateMaxLength);

    if (addresses->buildDate_ != gamesig::kVerifiedBuildDate) {
        // Единственная неудача, которую бессмысленно повторять: игра не станет
        // другой версии оттого, что мы попробуем ещё раз.
        failure = Failure::WrongBuild;

        error = std::format(
            "игра собрана \"{}\", а клиент выверен под \"{}\" — версии не совпадают",
            addresses->buildDate_, gamesig::kVerifiedBuildDate);
        return nullptr;
    }

    for (const gamesig::Signature& signature : gamesig::catalog()) {
        const gamesig::Resolved resolved = gamesig::resolve(image, signature);

        if (!resolved.ok()) {
            // Необязательная сигнатура уносит с собой только свою возможность.
            // Заводятся такие под разведку — то, что ещё не выверено на этой
            // сборке, — и ронять из-за них весь клиент незачем.
            if (!signature.required) {
                spdlog::warn("optional signature \"{}\" did not resolve: {} ({} matches)",
                             signature.id, gamesig::describe(resolved.status), resolved.matchCount);
                continue;
            }

            error = std::format("сигнатура \"{}\" не разрешилась: {} (совпадений {})", signature.id,
                                gamesig::describe(resolved.status), resolved.matchCount);
            return nullptr;
        }

        addresses->addresses_.emplace(signature.id, resolved.value);

        spdlog::debug("{} = {:#x}", signature.id, resolved.value);
    }

    spdlog::debug("движок опознан: сборка \"{}\", разрешено адресов: {}", addresses->buildDate_,
                 addresses->addresses_.size());

    return addresses;
}

std::uint64_t EngineAddresses::operator[](std::string_view id) const noexcept {
    const auto it = addresses_.find(std::string{id});
    return it == addresses_.end() ? 0 : it->second;
}

} // namespace oxymp::client::game
