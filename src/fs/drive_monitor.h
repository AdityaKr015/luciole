#pragma once

#include "fs/location.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace luciole {
  struct DriveUsage {
    std::uint64_t used = 0;
    std::uint64_t size = 0;
    bool operator==(const DriveUsage&) const = default;
  };

  // One mountable volume or mount reported by GIO's volume monitor (removable media, extra partitions,
  // network mounts), as Nautilus lists them under "Other Locations".
  struct Drive {
    // Stable while the device stays attached; identifies the drive across change notifications.
    std::string key;
    std::string label;
    std::string glyph;
    // Set while mounted.
    std::optional<Location> root;
    // Unmount or eject is available (the drive is mounted and GIO allows it).
    bool removable = false;
    // Ejecting, not just unmounting, is what removal does (removable media).
    bool ejectable = false;
    // Filesystem usage of the mounted root; null until queried, when unmounted, or when the size is unknown.
    std::optional<DriveUsage> usage;
  };

  // Main-context view of GIO's volume monitor. Callbacks run on the main context and must not throw.
  class DriveMonitor {
  public:
    DriveMonitor();
    ~DriveMonitor();
    DriveMonitor(const DriveMonitor&) = delete;
    DriveMonitor& operator=(const DriveMonitor&) = delete;

    // Starts GIO's volume monitor; drives() stays empty before. Blocks while GIO starts its monitor services,
    // so the application calls it after its first frame. Idempotent.
    void start();

    const std::vector<Drive>& drives() const;
    const Drive* find(const std::string& key) const;
    void setChangedCallback(std::function<void()> callback);
    // Re-queries usage of every mounted drive asynchronously; a drive with a query in flight is skipped.
    void refreshUsage();
    // Runs on the main context when a drive's usage changes. Must not throw.
    void setUsageChangedCallback(std::function<void()> callback);
    // Mounts the drive if needed; done receives the root, or an error message.
    void mount(const std::string& key, std::function<void(std::optional<Location>, std::string)> done);
    // Ejects removable media, otherwise unmounts; done receives an error message, empty on success.
    void remove(const std::string& key, std::function<void(std::string)> done);

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
