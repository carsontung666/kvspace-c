#pragma once

#include "kvspace/constants.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace kvspace {

// XValue owns a Go-compatible TLV value.
// Wire format: [1B kind_len][kind][4B array_len LE][4B raw_len LE][raw].
// None is the only zero-byte encoding.
class XValue {
public:
    XValue() = default;

    static XValue Null() { return {}; }

    static XValue Int8(std::int8_t value);
    static XValue Int16(std::int16_t value);
    static XValue Int32(std::int32_t value);
    static XValue Int64(std::int64_t value);
    static XValue Int(std::int64_t value) { return Int64(value); }

    static XValue Uint8(std::uint8_t value);
    static XValue Uint16(std::uint16_t value);
    static XValue Uint32(std::uint32_t value);
    static XValue Uint64(std::uint64_t value);

    static XValue Float32(float value);
    static XValue Float64(double value);
    static XValue Float(double value) { return Float64(value); }
    static XValue Bool(bool value);
    static XValue Str(std::string_view value);
    static XValue Bytes(const std::vector<std::uint8_t>& value);

    static XValue Raw(std::string_view kind_name,
                      const std::vector<std::uint8_t>& raw,
                      std::int32_t array_len = 1);
    static XValue Index(const std::vector<std::string>& children);
    static XValue LinkIndex(std::string_view target);
    static XValue ExtIndex(const std::vector<std::string>& children,
                           std::string_view ext_path);
    static bool IsKnownKind(std::string_view kind_name) noexcept;

    bool IsNull() const noexcept { return kind_.empty(); }
    const std::string& Kind() const noexcept { return kind_; }
    const std::vector<std::uint8_t>& RawBytes() const noexcept { return raw_; }
    std::int32_t ArrayLen() const noexcept { return array_len_; }
    std::int32_t ByteLen() const noexcept {
        return static_cast<std::int32_t>(raw_.size());
    }

    std::int8_t AsInt8() const;
    std::int16_t AsInt16() const;
    std::int32_t AsInt32() const;
    std::int64_t AsInt64() const;
    std::int64_t AsInt() const { return AsInt64(); }
    std::uint8_t AsUint8() const;
    std::uint16_t AsUint16() const;
    std::uint32_t AsUint32() const;
    std::uint64_t AsUint64() const;
    float AsFloat32() const;
    double AsFloat64() const;
    double AsFloat() const { return AsFloat64(); }
    bool AsBool() const;
    std::string AsStr() const;
    std::vector<std::uint8_t> AsBytes() const;

    std::vector<std::string> Children() const;
    std::string LinkTarget() const;
    std::string ExtPath() const;

    std::vector<std::uint8_t> Encode() const;
    static XValue Decode(const std::uint8_t* data, std::size_t len);
    static XValue Decode(const std::vector<std::uint8_t>& data) {
        return Decode(data.data(), data.size());
    }
    std::size_t EncodedSize() const noexcept;

    bool operator==(const XValue& other) const noexcept {
        return kind_ == other.kind_ && raw_ == other.raw_ &&
               array_len_ == other.array_len_;
    }
    bool operator!=(const XValue& other) const noexcept { return !(*this == other); }

private:
    XValue(std::string kind_name,
           std::vector<std::uint8_t> raw,
           std::int32_t array_len)
        : kind_(std::move(kind_name)), raw_(std::move(raw)), array_len_(array_len) {}

    std::string kind_;
    std::vector<std::uint8_t> raw_;
    std::int32_t array_len_ = 0;
};

} // namespace kvspace
