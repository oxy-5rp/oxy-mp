#include "blip_directory.hpp"

namespace oxymp::server {

shared::BlipId BlipDirectory::add(shared::BlipState state, std::size_t limit) {
    if (blips_.size() >= limit) {
        return shared::kInvalidBlipId;
    }

    // Счётчик начинается с единицы: ноль означает «метки нет», и выданный им
    // номер был бы неотличим от отсутствия.
    const shared::BlipId id = ++nextId_;

    state.id = id;
    blips_.emplace(id, Blip{.state = std::move(state)});

    return id;
}

bool BlipDirectory::update(shared::BlipId id, shared::BlipState state) {
    const auto found = blips_.find(id);
    if (found == blips_.end()) {
        return false;
    }

    // Номер — свой, а не присланный: его назначили при заведении, и позволить
    // правке его сменить значило бы разрешить метке стать другой меткой.
    state.id = id;
    found->second.state = std::move(state);

    return true;
}

bool BlipDirectory::setDimension(shared::BlipId id, std::int32_t dimension) {
    const auto found = blips_.find(id);
    if (found == blips_.end()) {
        return false;
    }

    found->second.dimension = dimension;
    return true;
}

bool BlipDirectory::remove(shared::BlipId id) {
    return blips_.erase(id) != 0;
}

const BlipDirectory::Blip* BlipDirectory::find(shared::BlipId id) const {
    const auto found = blips_.find(id);
    return found == blips_.end() ? nullptr : &found->second;
}

} // namespace oxymp::server
