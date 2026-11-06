#include "app/cli.h"
#include "core/application_identity.h"

#include <string_view>
#include <vector>

int main(int argc, char* argv[]) {
  setApplicationIdentity({.name = "noctalia-files", .envPrefix = "NOCTALIA_FILES"});
  std::vector<std::string_view> args;
  for (int index = 1; index < argc; ++index) {
    args.emplace_back(argv[index]);
  }
  return files::runCli(args);
}
