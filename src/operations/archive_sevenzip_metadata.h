#pragma once

#include "operations/archive_input.h"

namespace files::archive_work {
  std::vector<char> decodeSevenZipHeader(Input& input, Work& work, const Passwords& passwords);
}
