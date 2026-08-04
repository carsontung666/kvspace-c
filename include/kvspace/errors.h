#pragma once

#include <stdexcept>
#include <string>

namespace kvspace {

class Error : public std::runtime_error {
public:
    explicit Error(const std::string& message) : std::runtime_error(message) {}
};

#define KVSPACE_ERROR_TYPE(name, message)             \
    class name : public Error {                       \
    public:                                           \
        name() : Error(message) {}                    \
        explicit name(const std::string& detail)      \
            : Error(std::string(message) + ": " + detail) {} \
    }

KVSPACE_ERROR_TYPE(ErrInvalidPath, "kvspace: path must be absolute and canonical");
KVSPACE_ERROR_TYPE(ErrInvalidValue, "kvspace: invalid XValue");
KVSPACE_ERROR_TYPE(ErrInvalidDirectoryValue, "kvspace: directory value must be kind=index");
KVSPACE_ERROR_TYPE(ErrNotDirectory, "kvspace: parent path is not a directory");
KVSPACE_ERROR_TYPE(ErrDisconnected, "kvspace: connection is disconnected");
KVSPACE_ERROR_TYPE(ErrCapacity, "kvspace: shared region capacity exceeded");
KVSPACE_ERROR_TYPE(ErrCorruptRegion, "kvspace: corrupt shared region");
KVSPACE_ERROR_TYPE(ErrVersionMismatch, "kvspace: shared region ABI mismatch");
KVSPACE_ERROR_TYPE(ErrEngineMismatch, "kvspace: shared region engine mismatch");
KVSPACE_ERROR_TYPE(ErrUnsupportedEngine, "kvspace: unsupported shared region engine");
KVSPACE_ERROR_TYPE(ErrAlreadyExists, "kvspace: shared region already exists");
KVSPACE_ERROR_TYPE(ErrNotFound, "kvspace: shared region not found");
KVSPACE_ERROR_TYPE(ErrResolve, "kvspace: link resolution failed");
KVSPACE_ERROR_TYPE(ErrExtWrite, "kvspace: cannot write an extindex read-only path");
KVSPACE_ERROR_TYPE(ErrExtDelete, "kvspace: cannot delete an extindex read-only path");
KVSPACE_ERROR_TYPE(ErrExtCascade, "kvspace: ExtIndex cannot cascade");
KVSPACE_ERROR_TYPE(ErrExtTarget, "kvspace: ExtIndex target must be an ordinary index");
KVSPACE_ERROR_TYPE(ErrExtCollision, "kvspace: ExtIndex local and extension children overlap");
KVSPACE_ERROR_TYPE(ErrLinkTypeMismatch, "kvspace: link target and link path types differ");
KVSPACE_ERROR_TYPE(ErrLinkPathExists, "kvspace: link path contains a non-link value");

#undef KVSPACE_ERROR_TYPE

} // namespace kvspace
