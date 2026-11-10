#pragma once

#include "browser/action_registry.h"
#include "fs/cancel_token.h"
#include "platform/program_launcher.h"

#include <cstddef>
#include <memory>

class PageContext;
class SubmenuController;
class WaylandConnection;

class ExecutionController {
public:
  ExecutionController(PageContext& page, SubmenuController& submenus, WaylandConnection& connection);
  ~ExecutionController();
  void run(const files::ActionRequest& request, bool terminal);
  void scripts(const files::ActionRequest& request);
  void closeScripts();
  void cancel();
  bool busy() const { return m_pending != 0; }

private:
  void confirm(const files::ActionRequest& request, files::PreparedProgram program);
  void launch(const files::ActionRequest& request, files::PreparedProgram program, bool terminal, bool script);
  void showScripts(const files::ActionRequest& request, files::ScriptListing listing, bool loading);
  void openScriptsFolder(const files::ActionRequest& request);
  bool current(const files::ActionRequest& request) const;
  PageContext& m_page;
  SubmenuController& m_submenus;
  WaylandConnection& m_connection;
  files::CancelToken m_listing, m_launch;
  std::size_t m_pending = 0;
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
};
