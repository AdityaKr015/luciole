{ pkgs, settings }:
if builtins.isPath settings then
  settings
else if builtins.isString settings then
  pkgs.writeText "noctalia-files-config.toml" settings
else
  (pkgs.formats.toml { }).generate "noctalia-files-config.toml" settings
