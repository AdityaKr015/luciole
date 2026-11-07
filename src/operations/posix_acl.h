#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace files {
  enum class AclTag { Owner, User, OwningGroup, Group, Mask, Other };
  struct AclEntry {
    AclTag tag;
    std::string qualifier; // Numeric UID/GID on inspection; names or IDs in an edit.
    std::uint32_t permissions = 0;
    bool operator==(const AclEntry&) const = default;
  };
  struct AclInfo {
    std::vector<AclEntry> access, defaults;
    bool supported = false, editable = false, directory = false;
    std::string message;
  };
  struct AclEdit {
    std::optional<std::vector<AclEntry>> access, defaults;
  };
  inline constexpr std::size_t maxAclEntries = 128;
  // Worker-only Linux POSIX ACL adapter. Descriptor paths always address a held,
  // non-symlink inode; callers retain pathname/metadata validation policy.
  AclInfo inspectAcl(int descriptor);
  std::vector<AclEntry> normalizeAcl(
      std::vector<AclEntry> entries, bool allowEmpty,
      const std::function<std::uint32_t(AclTag, const std::string&)>& resolve
  );
  void writeAcl(int descriptor, bool defaults, const std::vector<AclEntry>& entries);
  bool sameAcl(const AclInfo& left, const AclInfo& right);
} // namespace files
