#pragma once

#include <optional>
#include <string>

namespace luciole {
  // `fileManagerActivation`: started by D-Bus for an org.freedesktop.FileManager1 call; open no initial window.
  int runApplication(
      const std::optional<std::string>& initialLocation = std::nullopt, bool fileManagerActivation = false
  );
} // namespace luciole
