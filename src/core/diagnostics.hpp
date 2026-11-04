#pragma once
// Error reporting without exceptions. geosim builds with -fno-exceptions and
// -fno-rtti (docs/09-engine-architecture.md#error-handling): loaders take a
// `diagnostics` sink, record the *first* error with its source:line, and
// return a neutral value. Callers check `failed()` at stage boundaries and
// stop early. Later errors are usually knock-on effects of the first, so only
// the first is kept.

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace geosim::core {

class diagnostics {
public:
    void fail(std::string message) {
        if(error_.empty()) {
            error_ = std::move(message);
        }
    }
    void fail(std::string_view source, std::size_t line, std::string_view message) {
        fail(std::string(source) + ":" + std::to_string(line) + ": " + std::string(message));
    }
    [[nodiscard]] bool failed() const noexcept { return !error_.empty(); }
    [[nodiscard]] std::string const& error() const noexcept { return error_; }

private:
    std::string error_;
};

} // namespace geosim::core
