#include "app/cli.h"

#include "app/application.h"
#include "config/settings.h"
#include "noctalia_files_build_info.h"
#include "noctalia_files_completions.h"
#include "thumbnails/thumbnail_worker.h"

#include <cstdio>
#include <cstdlib>
#include <print>

namespace {

  constexpr std::string_view kHelp =
      R"(Usage: noctalia-files [--help | --version | completions <shell> | config validate <file>]

Open Noctalia Files in Home, or pass one directory path/URI to browse.
Usage: noctalia-files [directory]

Options:
  --help                 Show this help
  --version              Show version and explicit build revision
  completions <shell>    Print bash, zsh, or fish shell completions
  config validate <file> Validate one TOML settings file without opening a window
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
  if (!args.empty() && args.front() == "--thumbnail-worker") {
    return runThumbnailWorker(args.subspan(1));
  }
  if (args.size() == 1 && args.front() == "--file-manager-service") {
    return runApplication(std::nullopt, true);
  }
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
  if (args.size() == 3 && args[0] == "config" && args[1] == "validate") {
    const auto issues = validateConfigFile(std::filesystem::path(args[2]));
    if (!issues.empty()) {
      for (const auto& issue : issues) {
        std::println(stderr, "noctalia-files: {}", issue.format());
      }
      return EXIT_FAILURE;
    }
    std::println("Configuration is valid.");
    return EXIT_SUCCESS;
  }
  if (args.empty()) {
    return runApplication();
  }
  if (args.size() == 1 && !args.front().starts_with('-') && args.front() != "config" && args.front() != "completions") {
    return runApplication(std::string(args.front()));
  }
  if (args.size() == 2 && args.front() == "--") {
    return runApplication(std::string(args[1]));
  }
  std::println(stderr, "noctalia-files: invalid arguments; use --help for available commands");
  return EXIT_FAILURE;
}
