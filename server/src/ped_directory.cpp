#include "ped_directory.hpp"

namespace oxymp::server {

shared::PedId PedDirectory::add(shared::PedState state, std::size_t limit) {
    if (state.model == 0 || peds_.size() >= limit) {
        return shared::kInvalidPedId;
    }

    const shared::PedId id = ++nextId_;

    state.id = id;
    peds_.emplace(id, Ped{.state = state});

    return id;
}

bool PedDirectory::update(shared::PedId id, shared::PedState state) {
    const auto found = peds_.find(id);
    if (found == peds_.end()) {
        return false;
    }

    // Номер и модель берутся у прохожего, а не у правки. Номер — потому что его
    // назначил сервер, и позволить правке его сменить значило бы разрешить кукле
    // стать другой куклой. Модель — потому что смена модели это не правка, а
    // новое тело: получателю пришлось бы убрать прохожего и завести заново, а он
    // об этом не узнает.
    state.id = id;
    state.model = found->second.state.model;

    found->second.state = state;
    return true;
}

bool PedDirectory::setDimension(shared::PedId id, std::int32_t dimension) {
    const auto found = peds_.find(id);
    if (found == peds_.end()) {
        return false;
    }

    found->second.dimension = dimension;
    return true;
}

bool PedDirectory::remove(shared::PedId id) {
    return peds_.erase(id) != 0;
}

const PedDirectory::Ped* PedDirectory::find(shared::PedId id) const {
    const auto it = peds_.find(id);
    return it == peds_.end() ? nullptr : &it->second;
}

} // namespace oxymp::server
