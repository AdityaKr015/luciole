#pragma once

#include <glib-object.h>
#include <memory>

namespace files {
  struct ObjectUnref {
    void operator()(gpointer object) const { g_object_unref(object); }
  };

  template <typename T> using GioPtr = std::unique_ptr<T, ObjectUnref>;

  struct GFree {
    void operator()(gpointer value) const { g_free(value); }
  };

  using GString = std::unique_ptr<char, GFree>;
} // namespace files
