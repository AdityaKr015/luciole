#include "core/application_identity.h"
namespace {
  const bool initialized = [] {
    setApplicationIdentity({.name = "noctalia-files", .envPrefix = "NOCTALIA_FILES"});
    return true;
  }();
} // namespace
