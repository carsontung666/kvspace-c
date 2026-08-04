#pragma once

#include <cstddef>

namespace kvspace::detail {

// Establishes implicit-lifetime objects over an existing byte representation.
// This is intentionally an out-of-line optimization barrier; see its CMake
// compile options before changing the implementation or call sites.
void StartImplicitLifetimes(void* storage, std::size_t storage_bytes) noexcept;

} // namespace kvspace::detail
