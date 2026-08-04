#include "test_support.h"

#include "kvspace/errors.h"
#include "kvspace/xvalue.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

void checkRoundTrip(const kvspace::XValue& value) {
    CHECK(kvspace::XValue::Decode(value.Encode()) == value);
    CHECK(value.EncodedSize() == value.Encode().size());
}

void testGoldenInt64() {
    const std::vector<std::uint8_t> expected{
        5, 'i', 'n', 't', '6', '4',
        1, 0, 0, 0,
        8, 0, 0, 0,
        42, 0, 0, 0, 0, 0, 0, 0};
    CHECK(kvspace::XValue::Int64(42).Encode() == expected);
    CHECK(kvspace::XValue::Decode(expected).AsInt64() == 42);
}

void testGoldenUtf8() {
    const auto value = kvspace::XValue::Str("你好");
    const auto encoded = value.Encode();
    CHECK(value.ArrayLen() == 2);
    CHECK(encoded[7] == 2);
    CHECK(encoded[11] == 6);
    CHECK(kvspace::XValue::Decode(encoded).AsStr() == "你好");

    const auto lone = kvspace::XValue::Raw("string", {0x80}, 1).Encode();
    CHECK(kvspace::XValue::Decode(lone).ArrayLen() == 1);
    CHECK(kvspace::XValue::Decode(lone).Encode() == lone);
    const auto overlong = kvspace::XValue::Raw("string", {0xC0, 0x80}, 2).Encode();
    CHECK(kvspace::XValue::Decode(overlong).ArrayLen() == 2);
    CHECK(kvspace::XValue::Decode(overlong).Encode() == overlong);
}

void testKinds() {
    checkRoundTrip(kvspace::XValue::Null());
    checkRoundTrip(kvspace::XValue::Int8(-7));
    checkRoundTrip(kvspace::XValue::Int16(-1234));
    checkRoundTrip(kvspace::XValue::Int32(-1234567));
    checkRoundTrip(kvspace::XValue::Int64(std::numeric_limits<std::int64_t>::min()));
    checkRoundTrip(kvspace::XValue::Uint8(250));
    checkRoundTrip(kvspace::XValue::Uint16(65000));
    checkRoundTrip(kvspace::XValue::Uint32(4000000000U));
    checkRoundTrip(kvspace::XValue::Uint64(std::numeric_limits<std::uint64_t>::max()));
    checkRoundTrip(kvspace::XValue::Float32(3.25F));
    checkRoundTrip(kvspace::XValue::Float64(-3.141592653589793));
    checkRoundTrip(kvspace::XValue::Bool(true));
    checkRoundTrip(kvspace::XValue::Str(""));
    checkRoundTrip(kvspace::XValue::Str("hello world"));
    checkRoundTrip(kvspace::XValue::Bytes({0, 1, 2, 255}));
    checkRoundTrip(kvspace::XValue::Raw("custom", {1, 2, 3}, 7));
    checkRoundTrip(kvspace::XValue::Index({"a", "b/"}));
    checkRoundTrip(kvspace::XValue::LinkIndex("/target/"));
    checkRoundTrip(kvspace::XValue::ExtIndex({"local", "dir/"}, "/base/"));

    CHECK(kvspace::XValue::Bool(true).AsBool());
    CHECK(!kvspace::XValue::Bool(false).AsBool());
    CHECK(std::abs(kvspace::XValue::Float32(3.25F).AsFloat32() - 3.25F) < 0.001F);
    CHECK(kvspace::XValue::Bytes({0, 1, 2}).AsBytes() ==
          std::vector<std::uint8_t>({0, 1, 2}));
    CHECK(kvspace::XValue::Bytes({'h', 'i'}).ArrayLen() == 1);
    const auto go_bytes = kvspace::XValue::Raw("bytes", {'h', 'i', 0}, 1).Encode();
    const auto decoded_bytes = kvspace::XValue::Decode(go_bytes);
    CHECK(decoded_bytes.ArrayLen() == 1);
    CHECK(decoded_bytes.Encode() == go_bytes);
    CHECK(kvspace::XValue::Index({"a", "b/"}).Children() ==
          std::vector<std::string>({"a", "b/"}));
    CHECK(kvspace::XValue::LinkIndex("/target/").LinkTarget() == "/target/");
    CHECK(kvspace::XValue::ExtIndex({"x"}, "/base/").ExtPath() == "/base/");
}

void testMalformed() {
    expectThrows<kvspace::ErrInvalidValue>([] {
        (void)kvspace::XValue::Decode(std::vector<std::uint8_t>{1, 'x'});
    });
    auto trailing = kvspace::XValue::Int64(1).Encode();
    trailing.push_back(0);
    expectThrows<kvspace::ErrInvalidValue>([&] {
        (void)kvspace::XValue::Decode(trailing);
    });
    expectThrows<kvspace::ErrInvalidValue>([] {
        (void)kvspace::XValue::Raw("bad-kind", {});
    });
    expectThrows<kvspace::ErrInvalidValue>([] {
        (void)kvspace::XValue::Int32(1).AsInt64();
    });

    const auto bool_two = kvspace::XValue::Raw("bool", {2}, 1);
    const auto canonical_bool = kvspace::XValue::Decode(bool_two.Encode());
    CHECK(canonical_bool.AsBool());
    CHECK(canonical_bool.RawBytes() == std::vector<std::uint8_t>({1}));
    CHECK(canonical_bool.Encode() != bool_two.Encode());

    const auto odd_int16 = kvspace::XValue::Raw("int16", {1, 2, 3}, 1);
    const auto canonical_int16 = kvspace::XValue::Decode(odd_int16.Encode());
    CHECK(canonical_int16.RawBytes() == std::vector<std::uint8_t>({1, 2}));
    CHECK(canonical_int16.Encode() != odd_int16.Encode());

    const auto malformed_ext = kvspace::XValue::Raw("extindex", {'/', 'x', '/'}, 1);
    CHECK(kvspace::XValue::Decode(malformed_ext.Encode()).RawBytes() ==
          kvspace::XValue::ExtIndex({}, "/x/").RawBytes());

    const auto go_time = kvspace::XValue::Raw(
        "time", {1, 2, 3, 4, 5, 6, 7, 8, 9}, 1);
    const auto decoded_time = kvspace::XValue::Decode(go_time.Encode());
    CHECK(decoded_time.ArrayLen() == 1);
    CHECK(decoded_time.RawBytes() == go_time.RawBytes());
    CHECK(decoded_time.Encode() == go_time.Encode());
}

} // namespace

int main() {
    return runTest([] {
        testGoldenInt64();
        testGoldenUtf8();
        testKinds();
        testMalformed();
    });
}
