#include "object_directory.hpp"

namespace oxymp::server {

shared::ObjectId ObjectDirectory::add(std::uint32_t model, const shared::Vec3& position,
                                      const shared::Vec3& rotation, std::size_t limit) {
    if (model == 0 || objects_.size() >= limit) {
        return shared::kInvalidObjectId;
    }

    const shared::ObjectId id = ++nextId_;

    objects_.emplace(id, Object{
                             .model = model,
                             .position = position,
                             .rotation = rotation,
                         });

    return id;
}

bool ObjectDirectory::remove(shared::ObjectId id) {
    return objects_.erase(id) != 0;
}

const ObjectDirectory::Object* ObjectDirectory::find(shared::ObjectId id) const {
    const auto it = objects_.find(id);
    return it == objects_.end() ? nullptr : &it->second;
}

} // namespace oxymp::server
