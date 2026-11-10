#include "core/application_identity.h"
namespace {
  const bool initialized = [] {
    setApplicationIdentity({.name = "luciole", .envPrefix = "LUCIOLE"});
    return true;
  }();
} // namespace
