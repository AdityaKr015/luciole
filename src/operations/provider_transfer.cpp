#include "operations/provider_transfer.h"

#include "fs/filename.h"
#include "fs/gio_interop.h"

#include <algorithm>
#include <format>
#include <set>
#include <utility>

namespace {
  using namespace luciole;
  constexpr const char* kAttributes = "standard::type,standard::is-symlink,standard::size,standard::symlink-target,"
                                      "id::file,id::filesystem,etag::value,time::modified,time::modified-usec,access::"
                                      "can-write,access::can-read,unix::mode";
  using Info = GioPtr<GFileInfo>;
  [[noreturn]] void fail(GError* raw, const char* operation = "Transfer") {
    const GErrorPtr error(raw);
    auto value = error ? fileError(*error) : FileError{FileErrorKind::Other, "Provider operation failed."};
    value.message = std::string(operation) + ": " + value.message;
    throw FileErrorException(std::move(value));
  }
  void check(const CancelToken& token) {
    if (token.cancelled()) {
      throwFileError(FileErrorKind::Cancelled, "Transfer cancelled.");
    }
  }
  Info info(const Location& location, const CancelToken& token, bool missing = false) {
    GError* error = nullptr;
    Info result(g_file_query_info(
        gioFile(location).get(), kAttributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, gioCancellable(token), &error
    ));
    if (!result) {
      if (missing && error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
        g_error_free(error);
        return {};
      }
      fail(error);
    }
    return result;
  }
  bool validName(std::string_view name) {
    return !name.empty() && name != "." && name != ".." && !name.contains('/') && !name.contains('\0');
  }
  void destinationName(const Location& parent, std::string_view name) {
    const bool smb = parent.uri().starts_with("smb:"),
               dav = parent.uri().starts_with("dav:") || parent.uri().starts_with("davs:");
    if ((smb || dav) && !g_utf8_validate(name.data(), static_cast<gssize>(name.size()), nullptr)) {
      throwFileError(
          FileErrorKind::InvalidArgument, "This destination requires UTF-8 filenames. The original name was retained."
      );
    }
    if (!smb) {
      return;
    }
    const auto base = name.substr(0, name.find('.'));
    const luciole::GString upper(g_ascii_strup(base.data(), static_cast<gssize>(base.size())));
    const std::string_view device(upper.get());
    const bool reserved = device == "CON"
        || device == "PRN"
        || device == "AUX"
        || device == "NUL"
        || (device.size() == 4
            && (device.starts_with("COM") || device.starts_with("LPT"))
            && device[3] >= '1'
            && device[3] <= '9');
    if (reserved || name.ends_with('.') || name.ends_with(' ') || std::ranges::any_of(name, [](unsigned char ch) {
          return ch < 0x20 || std::string_view("<>:\"\\|?*").contains(static_cast<char>(ch));
        })) {
      throwFileError(
          FileErrorKind::InvalidArgument,
          "This name cannot be represented on an SMB share. The original name was retained."
      );
    }
  }
  std::optional<std::uint64_t> fact(GFileInfo* value, const char* attribute) {
    return g_file_info_has_attribute(value, attribute)
        ? std::optional<std::uint64_t>(g_file_info_get_attribute_uint64(value, attribute))
        : std::nullopt;
  }
  bool folder(GFileInfo* value) {
    return g_file_info_get_file_type(value) == G_FILE_TYPE_DIRECTORY && !readIsSymlink(value);
  }
  void unchanged(GFileInfo* expected, GFileInfo* actual, bool content = true) {
    if (!actual
        || g_file_info_get_file_type(expected) != g_file_info_get_file_type(actual)
        || readIsSymlink(expected) != readIsSymlink(actual)) {
      throwFileError(FileErrorKind::Changed, "An item changed during the transfer.");
    }
    for (const char* attribute :
         {G_FILE_ATTRIBUTE_ID_FILE, G_FILE_ATTRIBUTE_ID_FILESYSTEM, G_FILE_ATTRIBUTE_ETAG_VALUE,
          G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_ATTRIBUTE_TIME_MODIFIED, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC}) {
      if (!content
          && attribute != std::string_view(G_FILE_ATTRIBUTE_ID_FILE)
          && attribute != std::string_view(G_FILE_ATTRIBUTE_ID_FILESYSTEM)) {
        continue;
      }
      if (!g_file_info_has_attribute(expected, attribute)) {
        continue;
      }
      const luciole::GString before(g_file_info_get_attribute_as_string(expected, attribute));
      const luciole::GString after(g_file_info_get_attribute_as_string(actual, attribute));
      if (!after || std::string_view(before.get()) != after.get()) {
        throwFileError(FileErrorKind::Changed, "An item changed during the transfer.");
      }
    }
  }
  std::vector<std::string> children(const Location& location, const CancelToken& token) {
    GError* error = nullptr;
    const GioPtr<GFileEnumerator> reader(g_file_enumerate_children(
        gioFile(location).get(), "standard::name", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, gioCancellable(token), &error
    ));
    if (!reader) {
      fail(error);
    }
    std::vector<std::string> names;
    while (true) {
      check(token);
      Info child(g_file_enumerator_next_file(reader.get(), gioCancellable(token), &error));
      if (!child) {
        if (error) {
          fail(error);
        }
        break;
      }
      const char* name = g_file_info_get_name(child.get());
      if (!name || !validName(name)) {
        throwFileError(FileErrorKind::InvalidArgument, "Invalid provider filename.");
      }
      names.emplace_back(name);
      if (names.size() > 100000) {
        throwFileError(FileErrorKind::Unsupported, "The folder exceeds the transfer entry limit.");
      }
    }
    if (!g_file_enumerator_close(reader.get(), gioCancellable(token), &error)) {
      fail(error);
    }
    std::ranges::sort(names);
    return names;
  }
  void mkdir(const Location& location, const CancelToken& token) {
    GError* error = nullptr;
    if (!g_file_make_directory(gioFile(location).get(), gioCancellable(token), &error)) {
      fail(error);
    }
  }
  void rename(const Location& source, const Location& destination, const CancelToken& token) {
    GError* error = nullptr;
    if (!g_file_move(
            gioFile(source).get(), gioFile(destination).get(),
            static_cast<GFileCopyFlags>(G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_NO_FALLBACK_FOR_MOVE),
            gioCancellable(token), nullptr, nullptr, &error
        )) {
      fail(error);
    }
  }
  void erase(const Location& location, const CancelToken& token) {
    GError* error = nullptr;
    if (!g_file_delete(gioFile(location).get(), gioCancellable(token), &error)) {
      fail(error);
    }
  }
  struct Tree {
    Location source;
    Info captured;
    std::vector<Tree> children;
  };
  Tree capture(const Location& location, const CancelToken& token, std::size_t& count, unsigned depth = 0) {
    check(token);
    if (++count > 100000 || depth > 128) {
      throwFileError(FileErrorKind::Unsupported, "The transfer exceeds the folder depth or entry limit.");
    }
    Tree tree{location, info(location, token), {}};
    const auto type = g_file_info_get_file_type(tree.captured.get());
    if (type != G_FILE_TYPE_REGULAR && type != G_FILE_TYPE_DIRECTORY && type != G_FILE_TYPE_SYMBOLIC_LINK) {
      throwFileError(FileErrorKind::Unsupported, "Special files cannot be transferred.");
    }
    if (folder(tree.captured.get())) {
      for (const auto& name : children(location, token)) {
        tree.children.push_back(capture(location.child(name), token, count, depth + 1));
      }
      unchanged(tree.captured.get(), info(location, token).get());
    }
    return tree;
  }
  void validate(const Tree& tree, const CancelToken& token) {
    check(token);
    unchanged(tree.captured.get(), info(tree.source, token).get());
    if (folder(tree.captured.get())) {
      const auto names = children(tree.source, token);
      std::vector<std::string> expected;
      expected.reserve(tree.children.size());
      for (const auto& child : tree.children) {
        expected.push_back(*child.source.basename());
        validate(child, token);
      }
      if (names != expected) {
        throwFileError(FileErrorKind::Changed, "Folder contents changed during the transfer.");
      }
    }
  }
  void copy(const Tree& tree, const Location& target, const CancelToken& token, std::atomic<std::uint64_t>& bytes) {
    check(token);
    destinationName(*target.parent(), *target.basename());
    unchanged(tree.captured.get(), info(tree.source, token).get());
    if (folder(tree.captured.get())) {
      mkdir(target, token);
      for (const auto& child : tree.children) {
        copy(child, target.child(*child.source.basename()), token, bytes);
      }
      GError* queryError = nullptr;
      const std::unique_ptr<GFileAttributeInfoList, decltype(&g_file_attribute_info_list_unref)> supported(
          g_file_query_settable_attributes(gioFile(target).get(), gioCancellable(token), &queryError),
          g_file_attribute_info_list_unref
      );
      if (!supported) {
        if (queryError && g_error_matches(queryError, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
          g_error_free(queryError);
          return;
        }
        fail(queryError);
      }
      Info attributes(g_file_info_new());
      for (const auto* attribute : {G_FILE_ATTRIBUTE_UNIX_MODE, G_FILE_ATTRIBUTE_TIME_MODIFIED}) {
        if (g_file_info_has_attribute(tree.captured.get(), attribute)
            && g_file_attribute_info_list_lookup(supported.get(), attribute)) {
          if (std::string_view(attribute) == G_FILE_ATTRIBUTE_UNIX_MODE) {
            g_file_info_set_attribute_uint32(
                attributes.get(), attribute, g_file_info_get_attribute_uint32(tree.captured.get(), attribute)
            );
          } else {
            g_file_info_set_attribute_uint64(
                attributes.get(), attribute, g_file_info_get_attribute_uint64(tree.captured.get(), attribute)
            );
          }
        }
      }
      GError* error = nullptr;
      if (!g_file_set_attributes_from_info(
              gioFile(target).get(), attributes.get(), G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, gioCancellable(token),
              &error
          )) {
        fail(error);
      }

    } else {
      GError* error = nullptr;
      std::pair<std::atomic<std::uint64_t>*, goffset> progress{&bytes, 0};
      if (!g_file_copy(
              gioFile(tree.source).get(), gioFile(target).get(), G_FILE_COPY_NOFOLLOW_SYMLINKS, gioCancellable(token),
              [](goffset current, goffset, gpointer data) {
                auto& progress = *static_cast<std::pair<std::atomic<std::uint64_t>*, goffset>*>(data);
                if (current > progress.second) {
                  progress.first->fetch_add(static_cast<std::uint64_t>(current - progress.second));
                  progress.second = current;
                }
              },
              &progress, &error
          )) {
        fail(error);
      }
      const auto written = info(target, token);
      if (g_file_info_get_file_type(tree.captured.get()) != g_file_info_get_file_type(written.get())
          || (g_file_info_get_file_type(written.get()) == G_FILE_TYPE_REGULAR
              && g_file_info_get_size(written.get()) != g_file_info_get_size(tree.captured.get()))) {
        throwFileError(FileErrorKind::Changed, "The provider did not report a complete copy.");
      }
    }
  }
  void removeSource(Tree& tree, const CancelToken& token) {
    check(token);
    unchanged(tree.captured.get(), info(tree.source, token).get(), !folder(tree.captured.get()));
    for (auto& child : tree.children) {
      removeSource(child, token);
    }
    erase(tree.source, token); // New children make this fail rather than deleting unobserved data.
  }
  // Cleanup is limited to a private staging tree owned by this job. It never follows links.
  void cleanup(const Location& location, unsigned depth = 0) {
    if (depth > 128) {
      throwFileError(FileErrorKind::Unsupported, "Staging cleanup depth exceeded.");
    }
    const CancelToken token;
    const auto value = info(location, token, true);
    if (!value) {
      return;
    }
    if (folder(value.get())) {
      for (const auto& name : children(location, token)) {
        cleanup(location.child(name), depth + 1);
      }
    }
    erase(location, token);
  }
  struct Node {
    Location source, parent;
    TransferResult result;
    Info captured, destinationStamp;
    std::vector<std::unique_ptr<Node>> children;
    bool merged = false, published = false;
    Node(Location source, Location parent)
        : source(std::move(source)), parent(std::move(parent)), result(this->source) {}
  };
  std::size_t retries(const Node& node) {
    if (node.result.retryable) {
      return 1;
    }
    std::size_t count = 0;
    for (const auto& child : node.children) {
      count += retries(*child);
    }
    return count;
  }
} // namespace

struct luciole::ProviderTransfer::State {
  TransferRequest request;
  std::vector<std::unique_ptr<Node>> roots;
  std::size_t entries = 0;
  explicit State(TransferRequest request) : request(std::move(request)) {}

  void execute(
      Node& node, const CancelToken& token, std::atomic<std::uint64_t>& bytes, const Choose& choose, unsigned depth = 0
  ) {
    if (node.result.outcome == TransferOutcome::Copied
        || node.result.outcome == TransferOutcome::Moved
        || node.result.outcome == TransferOutcome::Skipped) {
      return;
    }
    node.result.retryable = false;
    node.result.error.reset();
    try {
      check(token);
      if (depth > 128) {
        throwFileError(FileErrorKind::Unsupported, "The transfer folder depth limit was reached.");
      }
      auto source = info(node.source, token);
      if (node.captured) {
        unchanged(node.captured.get(), source.get(), !node.merged);
      } else {
        node.captured = std::move(source);
      }
      auto parent = info(node.parent, token);
      if (!folder(parent.get())) {
        throwFileError(FileErrorKind::NotDirectory, "The destination is not a folder.");
      }
      if (g_file_info_has_attribute(parent.get(), G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE)
          && !g_file_info_get_attribute_boolean(parent.get(), G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE)) {
        throwFileError(FileErrorKind::ReadOnly, "The destination is read-only.");
      }
      if (transferLocationsEqual(node.source, node.parent)
          || (folder(node.captured.get()) && transferLocationInside(node.parent, node.source))) {
        throwFileError(FileErrorKind::WouldRecurse, "A folder cannot be placed inside itself.");
      }
      auto name = node.source.basename();
      if (!name || !validName(*name)) {
        throwFileError(FileErrorKind::InvalidArgument, "A filesystem root cannot be transferred.");
      }
      destinationName(node.parent, *name);
      auto target = node.parent.child(*name);
      bool keepBoth = transferLocationsEqual(node.source, target) && request.action == TransferAction::Copy;
      if (transferLocationsEqual(node.source, target) && request.action == TransferAction::Move) {
        node.result.outcome = TransferOutcome::Skipped;
        return;
      }
      auto existing = info(target, token, true);
      std::uint64_t number = 0;
      while (keepBoth && existing) {
        if (++number > 10000) {
          throwFileError(FileErrorKind::Exists, "No unused destination filename was found.");
        }
        const auto stem = filenameStemLength(*name, folder(node.captured.get()));
        target = node.parent.child(name->substr(0, stem) + std::format(" ({})", number) + name->substr(stem));
        existing = info(target, token, true);
      }
      if (existing && !node.merged) {
        const bool folders = folder(node.captured.get()) && folder(existing.get());
        const auto choice = choose(
            {.id = 0,
             .source = node.source,
             .destination = target,
             .name = *target.displayName(),
             .sourceDirectory = folder(node.captured.get()),
             .destinationDirectory = folder(existing.get()),
             .sourceSize = folder(node.captured.get())
                 ? std::nullopt
                 : std::optional<std::uint64_t>(g_file_info_get_size(node.captured.get())),
             .destinationSize = folder(existing.get())
                 ? std::nullopt
                 : std::optional<std::uint64_t>(g_file_info_get_size(existing.get())),
             .sourceModified = fact(node.captured.get(), G_FILE_ATTRIBUTE_TIME_MODIFIED),
             .destinationModified = fact(existing.get(), G_FILE_ATTRIBUTE_TIME_MODIFIED),
             .canMerge = request.allowMerge && folders,
             .canApplyToAll = false,
             .scopeLabel = {}}
        );
        check(token);
        unchanged(node.captured.get(), info(node.source, token).get());
        unchanged(existing.get(), info(target, token).get());
        unchanged(parent.get(), info(node.parent, token).get(), false);
        if (choice == TransferConflictChoice::Cancel) {
          throwFileError(FileErrorKind::Cancelled, "Transfer cancelled.");
        }
        if (choice == TransferConflictChoice::Skip) {
          node.result.outcome = TransferOutcome::Skipped;
          return;
        }
        if (choice == TransferConflictChoice::KeepBoth) {
          // Retry name selection with the captured source; an occupied slot prompts again.
          for (std::uint64_t index = 1; index <= 10000; ++index) {
            const auto stem = filenameStemLength(*name, folder(node.captured.get()));
            target = node.parent.child(name->substr(0, stem) + std::format(" ({})", index) + name->substr(stem));
            existing = info(target, token, true);
            if (!existing) {
              break;
            }
          }
          if (existing) {
            throwFileError(FileErrorKind::Exists, "No unused destination filename was found.");
          }
        } else if (choice == TransferConflictChoice::Merge && request.allowMerge && folders) {
          node.merged = true;
          node.result.destination = target;
          node.destinationStamp = std::move(existing);
          for (const auto& child : children(node.source, token)) {
            if (++entries > 100000) {
              throwFileError(FileErrorKind::Unsupported, "The transfer entry limit was reached.");
            }
            node.children.push_back(std::make_unique<Node>(node.source.child(child), target));
          }
        } else if (choice != TransferConflictChoice::Replace) {
          throwFileError(FileErrorKind::Unsupported, "The selected conflict action is unavailable.");
        }
      }
      if (node.merged) {
        unchanged(node.destinationStamp.get(), info(*node.result.destination, token).get(), false);
        for (auto& child : node.children) {
          execute(*child, token, bytes, choose, depth + 1);
        }
        node.result.children.clear();
        bool complete = true;
        for (const auto& child : node.children) {
          node.result.children.push_back(child->result);
          complete &=
              child->result.outcome == TransferOutcome::Moved || child->result.outcome == TransferOutcome::Copied;
        }
        if (complete && request.action == TransferAction::Copy) {
          unchanged(node.captured.get(), info(node.source, token).get());
        }
        if (complete && request.action == TransferAction::Move) {
          erase(node.source, token);
        }
        node.result.outcome = complete
            ? (request.action == TransferAction::Move ? TransferOutcome::Moved : TransferOutcome::Copied)
            : TransferOutcome::Partial;
        node.result.error = complete
            ? std::nullopt
            : std::optional(FileError{FileErrorKind::Other, "Some folder contents were not transferred."});
        return;
      }
      // A private sibling stages the entire item before its final name becomes visible.
      const luciole::GString uuid(g_uuid_string_random());
      const auto stage = node.parent.child(std::string(".luciole-transfer-") + uuid.get());
      mkdir(stage, token);
      bool retain = false;
      try {
        std::size_t count = 0;
        auto tree = capture(node.source, token, count);
        const auto item = stage.child("item"), backup = stage.child("previous");
        copy(tree, item, token, bytes);
        validate(tree, token);
        unchanged(parent.get(), info(node.parent, token).get(), false);
        if (existing) {
          unchanged(existing.get(), info(target, token).get());
          rename(target, backup, token);
        }
        try {
          rename(item, target, token);
        } catch (...) {
          if (existing) {
            try {
              rename(backup, target, CancelToken{});
            } catch (...) {
              retain = true;
              node.published = true;
              node.result.destination = backup;
              throwFileError(
                  FileErrorKind::Other, "Publication failed; the previous destination remains in " + stage.uri()
              );
            }
          }
          throw;
        }
        node.result.destination = target;
        node.published = true;
        node.destinationStamp = info(target, token);
        if (request.action == TransferAction::Move) {
          removeSource(tree, token);
        }
        node.result.outcome = request.action == TransferAction::Move ? TransferOutcome::Moved : TransferOutcome::Copied;
        node.result.error.reset();
        cleanup(stage);
      } catch (...) {
        const auto error = currentFileError();
        if (!retain) {
          try {
            cleanup(stage);
          } catch (...) {
            throwFileError(FileErrorKind::Other, error.message + " Staging data remains in " + stage.uri());
          }
        }
        throw FileErrorException(error);
      }
    } catch (...) {
      node.result.error = currentFileError();
      node.result.outcome = node.published || node.merged       ? TransferOutcome::Partial
          : node.result.error->kind == FileErrorKind::Cancelled ? TransferOutcome::Cancelled
                                                                : TransferOutcome::Failed;
      node.result.retryable = !node.published
          && !node.merged
          && node.captured
          && node.result.error->kind != FileErrorKind::Changed
          && node.result.error->kind != FileErrorKind::Unsupported
          && node.result.error->kind != FileErrorKind::Cancelled;
    }
  }
};

luciole::ProviderTransfer::ProviderTransfer(TransferRequest request)
    : m_state(std::make_unique<State>(std::move(request))) {}
luciole::ProviderTransfer::~ProviderTransfer() = default;
std::size_t luciole::ProviderTransfer::retryable() const {
  std::size_t result = 0;
  for (const auto& node : m_state->roots) {
    result += retries(*node);
  }
  return result;
}
std::vector<luciole::TransferResult> luciole::ProviderTransfer::run(
    const CancelToken& token, std::atomic<std::uint64_t>& bytes, std::atomic<std::size_t>& completed,
    const Choose& choose
) {
  auto& state = *m_state;
  if (state.roots.empty()) {
    for (const auto& source : state.request.sources) {
      state.roots.push_back(std::make_unique<Node>(source, state.request.destination));
    }
  }
  std::vector<TransferResult> results;
  results.reserve(state.roots.size());
  std::set<std::string> seen;
  for (auto& node : state.roots) {
    try {
      if (state.request.sources.size() > 4096) {
        throwFileError(FileErrorKind::Unsupported, "Too many transfer sources.");
      }
      if (!transferLocationSupported(node->source)
          || !transferLocationSupported(node->parent)
          || state.request.action == TransferAction::Link) {
        throwFileError(
            FileErrorKind::Unsupported,
            "This transfer requires local or mounted SFTP, SMB or WebDAV locations; links require local paths."
        );
      }
      bool covered = !seen.insert(node->source.uri()).second;
      for (const auto& other : state.roots) {
        if (other.get() != node.get() && transferLocationInside(node->source, other->source)) {
          covered = true;
          break;
        }
      }
      if (covered) {
        node->result.outcome = TransferOutcome::Skipped;
      } else {
        state.execute(*node, token, bytes, choose);
      }
    } catch (...) {
      node->result.error = currentFileError();
    }
    results.push_back(node->result);
    completed.fetch_add(1);
  }
  return results;
}
