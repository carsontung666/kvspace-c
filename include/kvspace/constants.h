#pragma once

#include <string_view>

namespace kvspace {

inline constexpr std::string_view kPathSeparator = "/";
inline constexpr std::string_view kIndexSeparator = "\n";
inline constexpr std::string_view kExtIndexHead = "\xE2\x80\xA6"; // U+2026
inline constexpr std::size_t kMaxLinkResolutions = 64;

namespace kind {
inline constexpr std::string_view None = "";
inline constexpr std::string_view Bool = "bool";
inline constexpr std::string_view Int8 = "int8";
inline constexpr std::string_view Int16 = "int16";
inline constexpr std::string_view Int32 = "int32";
inline constexpr std::string_view Int64 = "int64";
inline constexpr std::string_view Uint8 = "uint8";
inline constexpr std::string_view Uint16 = "uint16";
inline constexpr std::string_view Uint32 = "uint32";
inline constexpr std::string_view Uint64 = "uint64";
inline constexpr std::string_view Float32 = "float32";
inline constexpr std::string_view Float64 = "float64";
inline constexpr std::string_view String = "string";
inline constexpr std::string_view Bytes = "bytes";
inline constexpr std::string_view Array1d = "array1d";
inline constexpr std::string_view Dict = "dict";
inline constexpr std::string_view Index = "index";
inline constexpr std::string_view LinkIndex = "linkindex";
inline constexpr std::string_view ExtIndex = "extindex";
inline constexpr std::string_view Rwir = "rwir";
inline constexpr std::string_view Rwfunc = "rwfunc";
inline constexpr std::string_view Scope = "scope";
inline constexpr std::string_view Time = "time";
inline constexpr std::string_view Duration = "duration";
} // namespace kind

} // namespace kvspace
