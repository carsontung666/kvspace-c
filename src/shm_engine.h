#pragma once

#include "kvspace/shm.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace kvspace::detail {

using EngineEntry = std::pair<std::string, std::vector<std::uint8_t>>;

// A prepared Clear owns every volatile scratch resource needed by its apply
// phase.  Apply may write persistent state, but must not allocate dynamic
// memory or perform any new validation whose failure could have been detected
// during PrepareClear.
class ShmPreparedClear {
public:
    virtual ~ShmPreparedClear() = default;
    virtual void Apply() && = 0;
};

// Owner-death recovery has the same two-phase contract as Clear, with a
// stronger failure policy enforced by Region: PrepareRecovery is read-only
// with respect to persistent bytes, while Apply owns all scratch resources and
// performs the recovery writes without allocating.  Region makes the robust
// mutex consistent only after Apply returns successfully.
class ShmPreparedRecovery {
public:
    virtual ~ShmPreparedRecovery() = default;
    virtual void Apply() && = 0;
};

template <typename Function>
class ShmPreparedRecoveryFunction final : public ShmPreparedRecovery {
public:
    explicit ShmPreparedRecoveryFunction(Function function)
        : function_(std::move(function)) {}

    void Apply() && override { function_(); }

private:
    Function function_;
};

template <typename Function>
std::unique_ptr<ShmPreparedRecovery> MakeShmPreparedRecovery(
    Function&& function) {
    using Stored = std::decay_t<Function>;
    return std::make_unique<ShmPreparedRecoveryFunction<Stored>>(
        std::forward<Function>(function));
}

template <typename Function>
class ShmPreparedClearFunction final : public ShmPreparedClear {
public:
    explicit ShmPreparedClearFunction(Function function)
        : function_(std::move(function)) {}

    void Apply() && override { function_(); }

private:
    Function function_;
};

template <typename Function>
std::unique_ptr<ShmPreparedClear> MakeShmPreparedClear(Function&& function) {
    using Stored = std::decay_t<Function>;
    return std::make_unique<ShmPreparedClearFunction<Stored>>(
        std::forward<Function>(function));
}

class ShmEngineStore {
public:
    virtual ~ShmEngineStore() = default;

    virtual ShmEngine Id() const noexcept = 0;
    virtual void Validate() const = 0;
    virtual bool Get(
        std::string_view key,
        std::vector<std::uint8_t>* value) const = 0;
    virtual bool Exists(std::string_view key) const = 0;
    virtual void Put(
        std::string_view key,
        const std::vector<std::uint8_t>& value) = 0;
    virtual bool Erase(std::string_view key) = 0;
    virtual std::vector<EngineEntry> Entries() const = 0;
    virtual std::vector<EngineEntry> EntriesWithPrefix(
        std::string_view prefix) const = 0;
    virtual std::unique_ptr<ShmPreparedClear> PrepareClear() = 0;
    virtual void Compact() = 0;

    virtual void BeginMutation() = 0;
    virtual void CommitMutation() = 0;
    virtual void RollbackMutation() noexcept = 0;
    virtual std::unique_ptr<ShmPreparedRecovery> PrepareRecovery() = 0;

    // Normal-operation rollback paths use this convenience wrapper. Robust
    // owner-death handling calls PrepareRecovery and Apply separately so it
    // can enforce the mutex poisoning/fail-stop boundary.
    void Recover() {
        auto prepared = PrepareRecovery();
        std::move(*prepared).Apply();
    }
};

} // namespace kvspace::detail
