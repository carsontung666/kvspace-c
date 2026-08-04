#include "shm_lifetime.h"

#include <cstring>

namespace kvspace::detail {

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void StartImplicitLifetimes(
    void* storage,
    std::size_t storage_bytes) noexcept {
    if (storage == nullptr || storage_bytes == 0) return;

    // Per the P0593 memmove semantics, this operation implicitly creates
    // suitable implicit-lifetime objects in the destination before copying.
    // Source and destination are deliberately identical, so optimized target
    // code need not read or write the persistent bytes.
    static_cast<void>(std::memmove(storage, storage, storage_bytes));
}

} // namespace kvspace::detail
