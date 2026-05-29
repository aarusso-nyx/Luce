#pragma once

#include <cstdint>

namespace luce {
namespace http {

enum class DispatchDecision : std::uint8_t {
  kInvoke = 0,
  kMethodNotAllowed,
  kUnauthorized,
};

inline bool route_method_allowed(std::uint16_t method_mask, int method) {
  if (method < 0 || method >= 16) {
    return false;
  }
  return (method_mask & static_cast<std::uint16_t>(1u << method)) != 0u;
}

inline DispatchDecision route_dispatch_decision(std::uint16_t method_mask, int method,
                                                bool requires_auth, bool authenticated) {
  if (!route_method_allowed(method_mask, method)) {
    return DispatchDecision::kMethodNotAllowed;
  }
  if (requires_auth && !authenticated) {
    return DispatchDecision::kUnauthorized;
  }
  return DispatchDecision::kInvoke;
}

} // namespace http
} // namespace luce
