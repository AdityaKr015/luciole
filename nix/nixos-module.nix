{ config, lib, ... }:
let
  cfg = config.programs.noctalia-files;
in
{
  options.programs.noctalia-files = {
    enable = lib.mkEnableOption "Noctalia Files";
    package = lib.mkOption {
      type = lib.types.nullOr lib.types.package;
      default = null;
      description = "Noctalia Files package to install.";
    };
  };

  config = lib.mkIf cfg.enable {
    environment.systemPackages = lib.optional (cfg.package != null) cfg.package;
  };
}
