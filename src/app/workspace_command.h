#pragma once

// Window and tab commands a page asks the workspace to carry out.
enum class WorkspaceCommand {
  ClosePane,
  NewTab,
  Duplicate,
  Close,
  Reopen,
  Split,
  SwitchArea,
  Swap,
  NewWindow,
  Detach,
  MoveWindow,
  OpenOther,
  CopyOther,
  MoveOther
};
