#pragma once

#include "fs/gio_ptr.h"

#include <gio/gio.h>
#include <optional>
#include <string>

namespace files {
  // URIs and native names are operation data, never presentation text.
  class Location {
  public:
    static Location fromUri(const std::string& uri);
    static Location fromPath(const std::string& absolutePath);
    static Location trash();

    [[nodiscard]] const std::string& uri() const { return m_uri; }
    [[nodiscard]] std::optional<std::string> nativePath() const;
    [[nodiscard]] bool isTrash() const;
    [[nodiscard]] GioPtr<GFile> file() const;

    bool operator==(const Location&) const = default;

  private:
    explicit Location(std::string uri);
    std::string m_uri;
  };
} // namespace files
