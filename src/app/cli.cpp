#include "app/cli.h"

#include "noctalia_files_build_info.h"
#include "noctalia_files_completions.h"

#include <cstdio>
#include <cstdlib>
#include <print>

namespace {

  constexpr std::string_view kHelp = R"(Usage: noctalia-files [--help | --version | completions <shell>]

Noctalia Files is under development. The graphical browser is not implemented yet.

Options:
  --help                 Show this help
  --version              Show version and explicit build revision
  completions <shell>    Print bash, zsh, or fish shell completions
)";

  int printCompletions(std::string_view shell) {
    if (shell == "bash") {
      std::print("{}", files::completions::kBash);
    } else if (shell == "zsh") {
      std::print("{}", files::completions::kZsh);
    } else if (shell == "fish") {
      std::print("{}", files::completions::kFish);
    } else {
      std::println(stderr, "noctalia-files: unsupported completion shell '{}'; expected bash, zsh, or fish", shell);
      return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
  }

} // namespace

int files::runCli(std::span<const std::string_view> args) {
  if (args.size() == 1 && args.front() == "--help") {
    std::print("{}", kHelp);
    return EXIT_SUCCESS;
  }
  if (args.size() == 1 && args.front() == "--version") {
    std::println("noctalia-files {} ({})", NOCTALIA_FILES_VERSION, NOCTALIA_FILES_BUILD_REVISION);
    return EXIT_SUCCESS;
  }
  if (args.size() == 2 && args.front() == "completions") {
    return printCompletions(args[1]);
  }
  if (args.empty()) {
    std::println(
        stderr, "noctalia-files: the graphical browser is not implemented yet; use --help for available commands"
    );
  } else {
    std::println(stderr, "noctalia-files: invalid arguments; use --help for available commands");
  }
  return EXIT_FAILURE;
}
