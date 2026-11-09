#pragma once

#include "operations/archive_input.h"

namespace files::archive_work {
  struct SevenZipMember {
    std::uint32_t index;
    std::string path;
    std::uint64_t size;
    unsigned mode;
    bool directory, encrypted;
  };
  std::string sevenZipWorker();
  int runSevenZip(
      Input& input, int output, std::uint64_t maximum, Work& work, std::string_view password,
      std::optional<std::uint32_t> index
  );
  std::vector<SevenZipMember> sevenZipCatalog(Input& input, Work& work, const Passwords& passwords);
} // namespace files::archive_work
