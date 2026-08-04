#pragma once

#include "kvspace/shm.h"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            std::ostringstream check_message;                                   \
            check_message << __FILE__ << ':' << __LINE__                        \
                          << ": check failed: " #condition;                    \
            throw std::runtime_error(check_message.str());                      \
        }                                                                       \
    } while (false)

template <typename ErrorType, typename Function>
void expectThrows(Function&& function) {
    bool caught = false;
    try {
        function();
    } catch (const ErrorType&) {
        caught = true;
    }
    CHECK(caught);
}

inline std::string uniqueShmName(std::string_view test) {
    return "/kvspace_" + std::string(test) + "_" +
           std::to_string(static_cast<long long>(::getpid()));
}

class ScopedRegion {
public:
    explicit ScopedRegion(std::string name) : name_(std::move(name)) {
        destroy();
    }
    ~ScopedRegion() { destroy(); }
    ScopedRegion(const ScopedRegion&) = delete;
    ScopedRegion& operator=(const ScopedRegion&) = delete;

    const std::string& name() const { return name_; }

private:
    void destroy() noexcept {
        try {
            kvspace::ShmClient::Destroy(name_);
        } catch (...) {
        }
    }

    std::string name_;
};

template <typename Function>
int runTest(Function&& function) {
    try {
        function();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
