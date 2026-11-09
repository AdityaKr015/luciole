#include "fs/cancel_token.h"

#include "fs/gio_interop.h"

struct files::CancelToken::State {
  GioPtr<GCancellable> cancellable{g_cancellable_new()};
};

struct files::CancelTokenAccess {
  static GCancellable* get(const CancelToken& token) { return token.m_state->cancellable.get(); }
};

files::CancelToken::CancelToken() : m_state(std::make_shared<State>()) {}
void files::CancelToken::cancel() const { g_cancellable_cancel(m_state->cancellable.get()); }
bool files::CancelToken::cancelled() const { return g_cancellable_is_cancelled(m_state->cancellable.get()) != FALSE; }

GCancellable* files::gioCancellable(const CancelToken& token) { return CancelTokenAccess::get(token); }
