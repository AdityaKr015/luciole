#include "operations/posix_acl.h"

#include <acl/libacl.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <sys/acl.h>
#include <sys/stat.h>
#include <tuple>
#include <type_traits>

namespace {
  struct FreeAcl {
    void operator()(void* value) const { ::acl_free(value); }
  };
  using Acl = std::unique_ptr<std::remove_pointer_t<acl_t>, FreeAcl>;
  constexpr std::array<acl_tag_t, 6> tags{ACL_USER_OBJ, ACL_USER, ACL_GROUP_OBJ, ACL_GROUP, ACL_MASK, ACL_OTHER};
  std::string path(int fd) {
    struct stat value{};
    if (::fstat(fd, &value) != 0) {
      throw std::runtime_error(std::strerror(errno));
    }
    if (!S_ISREG(value.st_mode) && !S_ISDIR(value.st_mode)) {
      throw std::runtime_error("ACL editing supports native regular files and folders, without following links.");
    }
    return "/proc/self/fd/" + std::to_string(fd);
  }
  void check(int result) {
    if (result != 0) {
      throw std::runtime_error(std::strerror(errno));
    }
  }
  std::uint32_t numeric(const std::string& text) {
    std::uint32_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty()
        || parsed.ec != std::errc{}
        || parsed.ptr != text.data() + text.size()
        || value == std::numeric_limits<std::uint32_t>::max()) {
      throw std::runtime_error("Invalid ACL account ID.");
    }
    return value;
  }
  void sort(std::vector<files::AclEntry>& entries) {
    std::ranges::sort(entries, [](const auto& left, const auto& right) {
      const auto id = [](const auto& entry) { return entry.qualifier.empty() ? 0U : numeric(entry.qualifier); };
      return std::tuple{left.tag, id(left)} < std::tuple{right.tag, id(right)};
    });
  }
  Acl build(const std::vector<files::AclEntry>& entries) {
    if (entries.size() > files::maxAclEntries) {
      throw std::runtime_error("The ACL editor supports at most 128 entries per list.");
    }
    Acl acl(::acl_init(static_cast<int>(entries.size())));
    if (!acl) {
      throw std::runtime_error(std::strerror(errno));
    }
    for (const auto& value : entries) {
      const auto tag = static_cast<std::size_t>(value.tag);
      if (tag >= tags.size() || value.permissions > 7) {
        throw std::runtime_error("Invalid ACL tag or permissions.");
      }
      const bool named = value.tag == files::AclTag::User || value.tag == files::AclTag::Group;
      if (!named && !value.qualifier.empty()) {
        throw std::runtime_error("Only named user/group entries accept an account.");
      }
      acl_entry_t entry = nullptr;
      auto* raw = acl.release();
      const int added = ::acl_create_entry(&raw, &entry);
      acl.reset(raw);
      check(added);
      check(::acl_set_tag_type(entry, tags[tag]));
      if (named) {
        const auto id = numeric(value.qualifier);
        if (value.tag == files::AclTag::User) {
          const uid_t user = id;
          check(::acl_set_qualifier(entry, &user));
        } else {
          const gid_t group = id;
          check(::acl_set_qualifier(entry, &group));
        }
      }
      acl_permset_t permissions = nullptr;
      check(::acl_get_permset(entry, &permissions));
      check(::acl_clear_perms(permissions));
      for (const auto bit : {ACL_READ, ACL_WRITE, ACL_EXECUTE}) {
        if ((value.permissions & bit) != 0) {
          check(::acl_add_perm(permissions, bit));
        }
      }
      check(::acl_set_permset(entry, permissions));
    }
    if (!entries.empty() && ::acl_valid(acl.get()) != 0) {
      throw std::runtime_error(
          "Invalid ACL: require owner, owning group and others; named accounts need a mask and unique IDs."
      );
    }
    return acl;
  }
  std::vector<files::AclEntry> read(const std::string& file, acl_type_t type) {
    Acl acl(::acl_get_file(file.c_str(), type));
    if (!acl) {
      throw std::runtime_error(std::strerror(errno));
    }
    const auto count = ::acl_entries(acl.get());
    if (count < 0 || count > static_cast<int>(files::maxAclEntries)) {
      throw std::runtime_error("The ACL exceeds the editor's 128-entry limit.");
    }
    std::vector<files::AclEntry> entries;
    acl_entry_t entry = nullptr;
    for (int next = ::acl_get_entry(acl.get(), ACL_FIRST_ENTRY, &entry); next != 0;
         next = ::acl_get_entry(acl.get(), ACL_NEXT_ENTRY, &entry)) {
      if (next < 0) {
        throw std::runtime_error(std::strerror(errno));
      }
      acl_tag_t tag = 0;
      check(::acl_get_tag_type(entry, &tag));
      const auto* const found = std::ranges::find(tags, tag);
      if (found == tags.end()) {
        throw std::runtime_error("Unsupported ACL entry type.");
      }
      files::AclEntry value{static_cast<files::AclTag>(found - tags.begin()), {}, 0};
      if (tag == ACL_USER || tag == ACL_GROUP) {
        const std::unique_ptr<void, FreeAcl> id(::acl_get_qualifier(entry));
        if (!id) {
          throw std::runtime_error(std::strerror(errno));
        }
        value.qualifier =
            std::to_string(tag == ACL_USER ? *static_cast<uid_t*>(id.get()) : *static_cast<gid_t*>(id.get()));
      }
      acl_permset_t permissions = nullptr;
      check(::acl_get_permset(entry, &permissions));
      for (const auto bit : {ACL_READ, ACL_WRITE, ACL_EXECUTE}) {
        const int present = ::acl_get_perm(permissions, bit);
        if (present < 0) {
          throw std::runtime_error(std::strerror(errno));
        }
        if (present != 0) {
          value.permissions |= bit;
        }
      }
      entries.push_back(std::move(value));
    }
    sort(entries);
    return entries;
  }
} // namespace
files::AclInfo files::inspectAcl(int descriptor) {
  AclInfo info;
  try {
    const auto file = path(descriptor);
    struct stat value{};
    if (::fstat(descriptor, &value) != 0) {
      throw std::runtime_error(std::strerror(errno));
    }
    info.directory = S_ISDIR(value.st_mode);
    info.access = read(file, ACL_TYPE_ACCESS);
    if (info.directory) {
      info.defaults = read(file, ACL_TYPE_DEFAULT);
    }
    info.supported = true;
  } catch (const std::exception& error) {
    info.message = "ACL unavailable: " + std::string(error.what());
  }
  return info;
}
std::vector<files::AclEntry> files::normalizeAcl(
    std::vector<AclEntry> entries, bool allowEmpty,
    const std::function<std::uint32_t(AclTag, const std::string&)>& resolve
) {
  if (!allowEmpty && entries.empty()) {
    throw std::runtime_error("The access ACL cannot be empty.");
  }
  if (entries.size() > maxAclEntries) {
    throw std::runtime_error("The ACL editor supports at most 128 entries per list.");
  }
  for (auto& entry : entries) {
    if (entry.tag == AclTag::User || entry.tag == AclTag::Group) {
      entry.qualifier = std::to_string(resolve(entry.tag, entry.qualifier));
    }
  }
  sort(entries);
  const auto checked = build(entries);
  return entries;
}
void files::writeAcl(int descriptor, bool defaults, const std::vector<AclEntry>& entries) {
  const auto file = path(descriptor);
  if (!defaults && entries.empty()) {
    throw std::runtime_error("The access ACL cannot be empty.");
  }
  const auto acl = build(entries);
  check(::acl_set_file(file.c_str(), defaults ? ACL_TYPE_DEFAULT : ACL_TYPE_ACCESS, acl.get()));
}
bool files::sameAcl(const AclInfo& left, const AclInfo& right) {
  return left.supported && right.supported && left.access == right.access && left.defaults == right.defaults;
}
