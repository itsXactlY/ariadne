#pragma once
// agent/errors.py — three exception classes; like Python's Exception(msg),
// each takes an optional message that what() returns.
// Base-class mapping preserves Python except-hierarchy classification:
//   Exception   -> std::exception
//   RuntimeError-> std::runtime_error
//   ValueError  -> std::invalid_argument (a std::logic_error)
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace dd {
namespace agent {

class SSLConfigurationError : public std::exception {
public:
    explicit SSLConfigurationError(std::string msg = "") : msg_(std::move(msg)) {}
    const char* what() const noexcept override { return msg_.c_str(); }

private:
    std::string msg_;
};

class EmptyStreamError : public std::runtime_error {
public:
    explicit EmptyStreamError(const std::string& msg = "") : std::runtime_error(msg) {}
};

class MoAPresetNotFoundError : public std::invalid_argument {
public:
    explicit MoAPresetNotFoundError(const std::string& msg = "") : std::invalid_argument(msg) {}
};

}  // namespace agent
}  // namespace dd
