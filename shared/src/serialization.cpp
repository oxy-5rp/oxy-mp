#include <oxymp/shared/protocol/serialization.hpp>

#include <algorithm>
#include <bit>
#include <cstring>

namespace oxymp::shared {
namespace {

/// Пишет целое от младшего байта к старшему.
///
/// Порядок задан явно, а не взят у платформы: обе стороны обязаны понимать
/// сообщение одинаково независимо от того, где собраны.
template<typename T>
void appendLittleEndian(std::vector<std::uint8_t>& bytes, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        bytes.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

template<typename T>
T loadLittleEndian(const std::uint8_t* data) noexcept {
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<T>(data[i]) << (i * 8);
    }
    return value;
}

} // namespace

void ByteWriter::writeU8(std::uint8_t value) {
    bytes_.push_back(value);
}

void ByteWriter::writeU16(std::uint16_t value) {
    appendLittleEndian(bytes_, value);
}

void ByteWriter::writeU32(std::uint32_t value) {
    appendLittleEndian(bytes_, value);
}

void ByteWriter::writeU64(std::uint64_t value) {
    appendLittleEndian(bytes_, value);
}

void ByteWriter::writeFloat(float value) {
    // Вещественное число передаётся своим двоичным представлением: обе стороны
    // используют IEEE 754, поэтому достаточно сохранить порядок байт.
    writeU32(std::bit_cast<std::uint32_t>(value));
}

void ByteWriter::writeVec3(const Vec3& value) {
    writeFloat(value.x);
    writeFloat(value.y);
    writeFloat(value.z);
}

void ByteWriter::writeString(std::string_view value) {
    const std::size_t length = std::min(value.size(), kMaxStringLength);

    writeU16(static_cast<std::uint16_t>(length));
    bytes_.insert(bytes_.end(), value.begin(), value.begin() + static_cast<std::ptrdiff_t>(length));
}

bool ByteReader::consume(std::size_t count) noexcept {
    if (failed_ || data_.size() - position_ < count) {
        failed_ = true;
        return false;
    }

    position_ += count;
    return true;
}

std::uint8_t ByteReader::readU8() noexcept {
    const std::size_t start = position_;
    if (!consume(sizeof(std::uint8_t))) {
        return 0;
    }
    return data_[start];
}

std::uint16_t ByteReader::readU16() noexcept {
    const std::size_t start = position_;
    if (!consume(sizeof(std::uint16_t))) {
        return 0;
    }
    return loadLittleEndian<std::uint16_t>(data_.data() + start);
}

std::uint32_t ByteReader::readU32() noexcept {
    const std::size_t start = position_;
    if (!consume(sizeof(std::uint32_t))) {
        return 0;
    }
    return loadLittleEndian<std::uint32_t>(data_.data() + start);
}

std::uint64_t ByteReader::readU64() noexcept {
    const std::size_t start = position_;
    if (!consume(sizeof(std::uint64_t))) {
        return 0;
    }
    return loadLittleEndian<std::uint64_t>(data_.data() + start);
}

float ByteReader::readFloat() noexcept {
    return std::bit_cast<float>(readU32());
}

Vec3 ByteReader::readVec3() noexcept {
    Vec3 value;
    value.x = readFloat();
    value.y = readFloat();
    value.z = readFloat();
    return value;
}

std::string ByteReader::readString() {
    const std::uint16_t length = readU16();

    if (length > kMaxStringLength) {
        failed_ = true;
        return {};
    }

    const std::size_t start = position_;
    if (!consume(length)) {
        return {};
    }

    return std::string{reinterpret_cast<const char*>(data_.data() + start), length};
}

} // namespace oxymp::shared
