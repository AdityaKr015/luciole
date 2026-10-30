{ pkgs, noctalia-files }:
pkgs.mkShell {
  inputsFrom = [ noctalia-files ];
  hardeningDisable = [ "fortify" ];

  nativeBuildInputs = with pkgs; [
    just
    meson
    ninja
    pkg-config
    wayland-scanner
    llvmPackages_22.clang-tools
    llvmPackages_22.libclang
    python3
    nixfmt
    statix
    deadnix
    gdb
    xdg-terminal-exec
    ffmpeg
    poppler-utils
  ];

  shellHook = ''
    export NOCTALIA_FILES_ASSETS_DIR="$PWD/assets"
    export GIO_EXTRA_MODULES="''${GIO_EXTRA_MODULES:+$GIO_EXTRA_MODULES:}${pkgs.gvfs}/lib/gio/modules"
    export XDG_DATA_DIRS="''${XDG_DATA_DIRS:-/usr/local/share:/usr/share}:${pkgs.adwaita-icon-theme}/share:${pkgs.shared-mime-info}/share"
    echo "Noctalia Files dev shell | just --list"
  '';
}
