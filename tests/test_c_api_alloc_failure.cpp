#include "kvspace/kshm.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace allocation_failure {

bool enabled = false;
bool arm_on_throw = false;

} // namespace allocation_failure

void* operator new(std::size_t size) {
    if (allocation_failure::enabled) throw std::bad_alloc();
    if (size == 0) size = 1;
    if (void* allocation = std::malloc(size)) return allocation;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void operator delete(void* allocation) noexcept { std::free(allocation); }

void operator delete[](void* allocation) noexcept { std::free(allocation); }

void operator delete(void* allocation, std::size_t) noexcept {
    std::free(allocation);
}

void operator delete[](void* allocation, std::size_t) noexcept {
    std::free(allocation);
}

using ExceptionDestructor = void (*)(void*);

extern "C" [[noreturn]] void __real___cxa_throw(
    void* exception,
    void* type,
    ExceptionDestructor destructor);

extern "C" [[noreturn]] void __wrap___cxa_throw(
    void* exception,
    void* type,
    ExceptionDestructor destructor) {
    if (allocation_failure::arm_on_throw) {
        allocation_failure::enabled = true;
    }
    __real___cxa_throw(exception, type, destructor);
}

namespace {

using ChildTest = int (*)();

int optionsNullDoesNotAllocate() {
    allocation_failure::enabled = true;
    try {
        kshm_options_init(nullptr);
    } catch (...) {
        allocation_failure::enabled = false;
        return 1;
    }
    allocation_failure::enabled = false;
    return std::strcmp(kshm_last_error(), "kvspace: null options") == 0 ? 0 : 2;
}

int exceptionTranslationDoesNotAllocate() {
    allocation_failure::arm_on_throw = true;
    const int status = kshm_disconnect(nullptr);
    allocation_failure::arm_on_throw = false;
    allocation_failure::enabled = false;
    if (status != KSHM_ERR_DISCONNECTED) return 1;
    return std::strcmp(
               kshm_last_error(), "kvspace: connection is disconnected") == 0
        ? 0
        : 2;
}

bool runChild(const char* name, ChildTest test) {
    const pid_t child = fork();
    if (child == -1) {
        std::perror("fork");
        return false;
    }
    if (child == 0) _exit(test());

    int status = 0;
    if (waitpid(child, &status, 0) != child) {
        std::perror("waitpid");
        return false;
    }
    if (!WIFEXITED(status)) {
        std::fprintf(stderr, "%s terminated by signal %d\n", name, WTERMSIG(status));
        return false;
    }
    if (WEXITSTATUS(status) != 0) {
        std::fprintf(
            stderr, "%s failed with exit code %d\n", name, WEXITSTATUS(status));
        return false;
    }
    return true;
}

} // namespace

int main() {
    const bool options_ok = runChild(
        "kshm_options_init(nullptr)", &optionsNullDoesNotAllocate);
    const bool translation_ok = runChild(
        "exception translation", &exceptionTranslationDoesNotAllocate);
    return options_ok && translation_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
