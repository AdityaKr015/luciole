#include "fs/file_query.h"

#include "fs/gio_interop.h"

#include <memory>
#include <utility>

void files::queryFileType(
    const Location& location, const CancelToken& token,
    std::move_only_function<void(std::expected<FileType, FileError>)> done
) {
  using Done = std::move_only_function<void(std::expected<FileType, FileError>)>;
  const auto file = gioFile(location);
  g_file_query_info_async(
      file.get(), G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, gioCancellable(token),
      [](GObject* source, GAsyncResult* result, gpointer raw) {
        const std::unique_ptr<Done> done(static_cast<Done*>(raw));
        GError* rawError = nullptr;
        const GioPtr<GFileInfo> info(g_file_query_info_finish(G_FILE(source), result, &rawError));
        const GErrorPtr error(rawError);
        if (error && g_error_matches(error.get(), G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
          return;
        }
        if (error) {
          (*done)(std::unexpected(fileError(*error)));
        } else {
          (*done)(fileType(g_file_info_get_file_type(info.get())));
        }
      },
      new Done(std::move(done))
  );
}

std::optional<files::FileError> files::enumerateChildren(
    const Location& parent, const CancelToken& token, const std::function<bool(const ChildInfo&)>& visit
) {
  auto* cancellable = gioCancellable(token);
  GError* raw = nullptr;
  const GioPtr<GFileEnumerator> enumerator(g_file_enumerate_children(
      gioFile(parent).get(),
      G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE
                                     "," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
      G_FILE_QUERY_INFO_NONE, cancellable, &raw
  ));
  while (enumerator && !g_cancellable_is_cancelled(cancellable)) {
    const GioPtr<GFileInfo> info(g_file_enumerator_next_file(enumerator.get(), cancellable, &raw));
    if (!info) {
      break;
    }
    const char* name = g_file_info_get_name(info.get());
    const char* display = g_file_info_get_display_name(info.get());
    const ChildInfo child{
        .name = name,
        .displayName = display ? display : name,
        .type = fileType(g_file_info_get_file_type(info.get())),
        .hidden = g_file_info_get_is_hidden(info.get()) != FALSE,
    };
    if (!visit(child)) {
      break;
    }
  }
  const GErrorPtr error(raw);
  if (enumerator) {
    g_file_enumerator_close(enumerator.get(), cancellable, nullptr);
  }
  if (error && !g_error_matches(error.get(), G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    return fileError(*error);
  }
  return std::nullopt;
}
