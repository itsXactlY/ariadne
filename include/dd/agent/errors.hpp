#pragma once
// agent/errors.py — three exception classes (pure port; no payload).
// Base-class mapping preserves Python except-hierarchy classification:
//   Exception   -> std::exception
//   RuntimeError-> std::runtime_error
//   ValueError  -> std::logic_error
#include <exception>
#include <stdexcept>

namespace dd {
namespace agent {

class SSLConfigurationError : public std::exception {
public:
    SSLConfigurationError() = default;
    const char* what() const noexcept override { return ""; }
};

class EmptyStreamError : public std::runtime_error {
public:
    EmptyStreamError() : std::runtime_error("") {}
    const char* what() const noexcept override { return ""; }
};

class MoAPresetNotFoundError : public std::logic_error {
public:
    MoAPresetNotFoundError() : std::logic_error("") {}
    const char* what() const noexcept override { return ""; }
};

}  // namespace agent
}  // namespace dd
