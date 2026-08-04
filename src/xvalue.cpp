#include "kvspace/xvalue.h"
#include "kvspace/errors.h"

#include <algorithm>
#include <limits>
#include <type_traits>

namespace kvspace {
namespace {

template <typename UInt>
std::vector<std::uint8_t> littleEndian(UInt value) {
    static_assert(std::is_unsigned<UInt>::value, "UInt must be unsigned");
    std::vector<std::uint8_t> out(sizeof(UInt));
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        out[i] = static_cast<std::uint8_t>(value >> (i * 8U));
    }
    return out;
}

template <typename UInt>
UInt readLittleEndian(const std::uint8_t* data) {
    static_assert(std::is_unsigned<UInt>::value, "UInt must be unsigned");
    UInt value = 0;
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        const auto byte = static_cast<UInt>(
            static_cast<UInt>(data[i]) << (i * 8U));
        value = static_cast<UInt>(value | byte);
    }
    return value;
}

bool validKind(std::string_view value) {
    if (value.empty() || value.size() > 255) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_';
    });
}

std::int32_t utf8Length(std::string_view value) {
    std::uint64_t count = 0;
    const auto continuation = [&](std::size_t index) {
        return index < value.size() &&
            (static_cast<unsigned char>(value[index]) & 0xC0U) == 0x80U;
    };
    for (std::size_t i = 0; i < value.size();) {
        const auto lead = static_cast<unsigned char>(value[i]);
        std::size_t width = 1;
        if (lead < 0x80U) {
            width = 1;
        } else if (lead >= 0xC2U && lead <= 0xDFU && continuation(i + 1)) {
            width = 2;
        } else if (lead >= 0xE0U && lead <= 0xEFU &&
                   continuation(i + 1) && continuation(i + 2)) {
            const auto second = static_cast<unsigned char>(value[i + 1]);
            const bool shortest = lead != 0xE0U || second >= 0xA0U;
            const bool not_surrogate = lead != 0xEDU || second < 0xA0U;
            if (shortest && not_surrogate) width = 3;
        } else if (lead >= 0xF0U && lead <= 0xF4U &&
                   continuation(i + 1) && continuation(i + 2) &&
                   continuation(i + 3)) {
            const auto second = static_cast<unsigned char>(value[i + 1]);
            const bool shortest = lead != 0xF0U || second >= 0x90U;
            const bool in_range = lead != 0xF4U || second < 0x90U;
            if (shortest && in_range) width = 4;
        }
        i += width;
        ++count;
    }
    if (count > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int32_t>::max())) {
        throw ErrInvalidValue("string has too many runes");
    }
    return static_cast<std::int32_t>(count);
}

std::vector<std::uint8_t> bytes(std::string_view value) {
    return {value.begin(), value.end()};
}

std::int32_t bytesArrayLength(const std::vector<std::uint8_t>& raw) {
    const auto separators = std::count(raw.begin(), raw.end(), std::uint8_t{0});
    if (separators == 0 && !raw.empty()) return 1;
    if (separators > static_cast<std::vector<std::uint8_t>::difference_type>(
                         std::numeric_limits<std::int32_t>::max())) {
        throw ErrInvalidValue("bytes array is too large");
    }
    return static_cast<std::int32_t>(separators);
}

std::string join(const std::vector<std::string>& values) {
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out.push_back('\n');
        out += values[i];
    }
    return out;
}

std::vector<std::string> split(std::string_view value) {
    if (value.empty()) return {};
    std::vector<std::string> out;
    for (std::size_t start = 0;;) {
        const auto end = value.find('\n', start);
        out.emplace_back(value.substr(start, end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return out;
}

void require(const XValue& value, std::string_view want, std::size_t bytes_needed) {
    if (value.Kind() != want || value.RawBytes().size() != bytes_needed) {
        throw ErrInvalidValue(std::string("expected ") + std::string(want));
    }
}

std::int32_t canonicalizeKnownRaw(
    std::string_view kind_name,
    std::vector<std::uint8_t>* raw,
    std::int32_t encoded) {
    if (kind_name == kind::String) {
        return utf8Length(std::string_view(
            reinterpret_cast<const char*>(raw->data()), raw->size()));
    }
    if (kind_name == kind::Bytes) {
        return bytesArrayLength(*raw);
    }
    if (kind_name == kind::Bool) {
        for (auto& byte : *raw) byte = byte == 0 ? 0 : 1;
        return static_cast<std::int32_t>(raw->size());
    }
    if (kind_name == kind::Int8 || kind_name == kind::Uint8) {
        return static_cast<std::int32_t>(raw->size());
    }
    if (kind_name == kind::Time) {
        return static_cast<std::int32_t>(raw->size() / 8);
    }
    std::size_t width = 0;
    if (kind_name == kind::Int16 || kind_name == kind::Uint16) width = 2;
    if (kind_name == kind::Int32 || kind_name == kind::Uint32 ||
        kind_name == kind::Float32) width = 4;
    if (kind_name == kind::Int64 || kind_name == kind::Uint64 ||
        kind_name == kind::Float64 || kind_name == kind::Duration) width = 8;
    if (width != 0) {
        raw->resize(raw->size() / width * width);
        return static_cast<std::int32_t>(raw->size() / width);
    }
    if (kind_name == kind::Dict) {
        raw->clear();
        return 1;
    }
    if (kind_name == kind::ExtIndex) {
        const std::string body(raw->begin(), raw->end());
        const auto newline = body.find('\n');
        const auto first = body.substr(0, newline);
        const std::string head(kExtIndexHead);
        const auto ext_path = first.rfind(head, 0) == 0
            ? first.substr(head.size())
            : first;
        std::string canonical = head + ext_path;
        if (newline != std::string::npos) canonical += body.substr(newline);
        *raw = bytes(canonical);
        return 1;
    }
    if (kind_name == kind::Index || kind_name == kind::LinkIndex ||
        kind_name == kind::Rwir) {
        return 1;
    }
    return encoded;
}

} // namespace

XValue XValue::Int8(std::int8_t value) {
    return Raw(kind::Int8, {static_cast<std::uint8_t>(value)});
}
XValue XValue::Int16(std::int16_t value) {
    return Raw(kind::Int16, littleEndian(static_cast<std::uint16_t>(value)));
}
XValue XValue::Int32(std::int32_t value) {
    return Raw(kind::Int32, littleEndian(static_cast<std::uint32_t>(value)));
}
XValue XValue::Int64(std::int64_t value) {
    return Raw(kind::Int64, littleEndian(static_cast<std::uint64_t>(value)));
}
XValue XValue::Uint8(std::uint8_t value) { return Raw(kind::Uint8, {value}); }
XValue XValue::Uint16(std::uint16_t value) { return Raw(kind::Uint16, littleEndian(value)); }
XValue XValue::Uint32(std::uint32_t value) { return Raw(kind::Uint32, littleEndian(value)); }
XValue XValue::Uint64(std::uint64_t value) { return Raw(kind::Uint64, littleEndian(value)); }

XValue XValue::Float32(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return Raw(kind::Float32, littleEndian(bits));
}
XValue XValue::Float64(double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return Raw(kind::Float64, littleEndian(bits));
}
XValue XValue::Bool(bool value) {
    return Raw(kind::Bool, {static_cast<std::uint8_t>(value ? 1 : 0)});
}
XValue XValue::Str(std::string_view value) {
    return XValue(std::string(kind::String), bytes(value), utf8Length(value));
}
XValue XValue::Bytes(const std::vector<std::uint8_t>& value) {
    return XValue(std::string(kind::Bytes), value, bytesArrayLength(value));
}
XValue XValue::Raw(std::string_view kind_name,
                   const std::vector<std::uint8_t>& raw,
                   std::int32_t array_len) {
    if (!validKind(kind_name)) throw ErrInvalidValue("invalid kind");
    if (raw.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw ErrInvalidValue("raw value is too large");
    }
    if (array_len < 0) throw ErrInvalidValue("negative array length");
    return XValue(std::string(kind_name), raw, array_len);
}
XValue XValue::Index(const std::vector<std::string>& children) {
    return Raw(kind::Index, bytes(join(children)));
}
XValue XValue::LinkIndex(std::string_view target) {
    return Raw(kind::LinkIndex, bytes(target));
}
XValue XValue::ExtIndex(const std::vector<std::string>& children,
                        std::string_view ext_path) {
    std::string raw(kExtIndexHead);
    raw += ext_path;
    if (!children.empty()) {
        raw.push_back('\n');
        raw += join(children);
    }
    return Raw(kind::ExtIndex, bytes(raw));
}

bool XValue::IsKnownKind(std::string_view kind_name) noexcept {
    return kind_name == kind::Bool ||
        kind_name == kind::Int8 || kind_name == kind::Int16 ||
        kind_name == kind::Int32 || kind_name == kind::Int64 ||
        kind_name == kind::Uint8 || kind_name == kind::Uint16 ||
        kind_name == kind::Uint32 || kind_name == kind::Uint64 ||
        kind_name == kind::Float32 || kind_name == kind::Float64 ||
        kind_name == kind::String || kind_name == kind::Bytes ||
        kind_name == kind::Dict || kind_name == kind::Index ||
        kind_name == kind::LinkIndex || kind_name == kind::ExtIndex ||
        kind_name == kind::Rwir || kind_name == kind::Rwfunc ||
        kind_name == kind::Time || kind_name == kind::Duration;
}

std::int8_t XValue::AsInt8() const {
    require(*this, kind::Int8, 1); return static_cast<std::int8_t>(raw_[0]);
}
std::int16_t XValue::AsInt16() const {
    require(*this, kind::Int16, 2);
    return static_cast<std::int16_t>(readLittleEndian<std::uint16_t>(raw_.data()));
}
std::int32_t XValue::AsInt32() const {
    require(*this, kind::Int32, 4);
    return static_cast<std::int32_t>(readLittleEndian<std::uint32_t>(raw_.data()));
}
std::int64_t XValue::AsInt64() const {
    require(*this, kind::Int64, 8);
    return static_cast<std::int64_t>(readLittleEndian<std::uint64_t>(raw_.data()));
}
std::uint8_t XValue::AsUint8() const { require(*this, kind::Uint8, 1); return raw_[0]; }
std::uint16_t XValue::AsUint16() const {
    require(*this, kind::Uint16, 2); return readLittleEndian<std::uint16_t>(raw_.data());
}
std::uint32_t XValue::AsUint32() const {
    require(*this, kind::Uint32, 4); return readLittleEndian<std::uint32_t>(raw_.data());
}
std::uint64_t XValue::AsUint64() const {
    require(*this, kind::Uint64, 8); return readLittleEndian<std::uint64_t>(raw_.data());
}
float XValue::AsFloat32() const {
    require(*this, kind::Float32, 4);
    const auto bits = readLittleEndian<std::uint32_t>(raw_.data());
    float value = 0; std::memcpy(&value, &bits, sizeof(value)); return value;
}
double XValue::AsFloat64() const {
    require(*this, kind::Float64, 8);
    const auto bits = readLittleEndian<std::uint64_t>(raw_.data());
    double value = 0; std::memcpy(&value, &bits, sizeof(value)); return value;
}
bool XValue::AsBool() const {
    require(*this, kind::Bool, 1);
    if (raw_[0] > 1) throw ErrInvalidValue("invalid bool payload");
    return raw_[0] != 0;
}
std::string XValue::AsStr() const {
    if (kind_ != kind::String) throw ErrInvalidValue("expected string");
    return {raw_.begin(), raw_.end()};
}
std::vector<std::uint8_t> XValue::AsBytes() const {
    if (kind_ != kind::Bytes) throw ErrInvalidValue("expected bytes");
    return raw_;
}

std::vector<std::string> XValue::Children() const {
    if (kind_ == kind::Index) {
        return split(std::string_view(reinterpret_cast<const char*>(raw_.data()), raw_.size()));
    }
    if (kind_ != kind::ExtIndex) throw ErrInvalidValue("expected index");
    const auto all = split(std::string_view(reinterpret_cast<const char*>(raw_.data()), raw_.size()));
    if (all.empty()) return {};
    return {all.begin() + 1, all.end()};
}
std::string XValue::LinkTarget() const {
    if (kind_ != kind::LinkIndex) throw ErrInvalidValue("expected linkindex");
    return {raw_.begin(), raw_.end()};
}
std::string XValue::ExtPath() const {
    if (kind_ != kind::ExtIndex) throw ErrInvalidValue("expected extindex");
    const auto newline = std::find(raw_.begin(), raw_.end(), static_cast<std::uint8_t>('\n'));
    std::string first(raw_.begin(), newline);
    const std::string head(kExtIndexHead);
    if (first.rfind(head, 0) != 0) throw ErrInvalidValue("invalid extindex head");
    return first.substr(head.size());
}

std::vector<std::uint8_t> XValue::Encode() const {
    if (IsNull()) return {};
    const auto kind_len = kind_.size();
    std::vector<std::uint8_t> out(1 + kind_len + 8 + raw_.size());
    out[0] = static_cast<std::uint8_t>(kind_len);
    std::memcpy(out.data() + 1, kind_.data(), kind_len);
    const std::uint32_t array_len = static_cast<std::uint32_t>(
        array_len_ <= 0 ? 1 : array_len_);
    const std::uint32_t raw_len = static_cast<std::uint32_t>(raw_.size());
    const auto array_bytes = littleEndian(array_len);
    const auto raw_bytes = littleEndian(raw_len);
    std::memcpy(out.data() + 1 + kind_len, array_bytes.data(), 4);
    std::memcpy(out.data() + 1 + kind_len + 4, raw_bytes.data(), 4);
    if (!raw_.empty()) {
        std::memcpy(out.data() + 1 + kind_len + 8, raw_.data(), raw_.size());
    }
    return out;
}

XValue XValue::Decode(const std::uint8_t* data, std::size_t len) {
    if (len == 0) return Null();
    if (data == nullptr) throw ErrInvalidValue("null TLV pointer");
    const std::size_t kind_len = data[0];
    if (kind_len == 0 || len < 1 + kind_len + 8) {
        throw ErrInvalidValue("truncated TLV header");
    }
    const std::string kind_name(reinterpret_cast<const char*>(data + 1), kind_len);
    if (!validKind(kind_name)) throw ErrInvalidValue("invalid TLV kind");
    const auto encoded_array_len = static_cast<std::int32_t>(
        readLittleEndian<std::uint32_t>(data + 1 + kind_len));
    const auto raw_len = readLittleEndian<std::uint32_t>(data + 1 + kind_len + 4);
    const std::size_t header_len = 1 + kind_len + 8;
    if (raw_len > len - header_len || header_len + raw_len != len) {
        throw ErrInvalidValue("TLV length mismatch");
    }
    std::vector<std::uint8_t> raw(data + header_len, data + len);
    const auto array_len = canonicalizeKnownRaw(kind_name, &raw, encoded_array_len);
    return XValue(kind_name, std::move(raw), array_len);
}

std::size_t XValue::EncodedSize() const noexcept {
    return IsNull() ? 0 : 1 + kind_.size() + 8 + raw_.size();
}

} // namespace kvspace
