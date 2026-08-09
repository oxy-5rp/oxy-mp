#include <oxymp/memscan/scanner.hpp>

#include <cstring>

namespace oxymp::memscan {

std::vector<std::size_t> findAll(ByteView haystack, const Pattern& pattern, std::size_t limit) {
    std::vector<std::size_t> hits;

    const std::size_t patternSize = pattern.size();
    if (patternSize == 0 || haystack.size() < patternSize) {
        return hits;
    }

    const std::uint8_t* const base = haystack.data();
    const std::size_t anchorIndex = pattern.anchorIndex();
    const std::uint8_t anchorByte = pattern.anchorByte();

    // Позиция якорного байта для последнего допустимого начала совпадения.
    const std::size_t lastAnchorPos = haystack.size() - patternSize + anchorIndex;

    std::size_t pos = anchorIndex;
    while (pos <= lastAnchorPos) {
        const void* found = std::memchr(base + pos, anchorByte, lastAnchorPos - pos + 1);
        if (found == nullptr) {
            break;
        }
        pos = static_cast<std::size_t>(static_cast<const std::uint8_t*>(found) - base);

        const std::size_t candidate = pos - anchorIndex;
        if (pattern.matchesAt(base + candidate)) {
            hits.push_back(candidate);
            if (limit != 0 && hits.size() >= limit) {
                break;
            }
        }

        ++pos;
    }

    return hits;
}

std::optional<std::size_t> findFirst(ByteView haystack, const Pattern& pattern) {
    const auto hits = findAll(haystack, pattern, 1);
    if (hits.empty()) {
        return std::nullopt;
    }
    return hits.front();
}

} // namespace oxymp::memscan
