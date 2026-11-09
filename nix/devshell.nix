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
    llvmPackages_23.clang-tools
    llvmPackages_23.libclang
    noctalia-files.archivePython
    nixfmt
    statix
    deadnix
    gdb
    xdg-terminal-exec
    ffmpeg
    poppler-utils
    _7zz
  ];

  shellHook = ''
    export NOCTALIA_FILES_ASSETS_DIR="$PWD/lib/assets"
    export GIO_EXTRA_MODULES="''${GIO_EXTRA_MODULES:+$GIO_EXTRA_MODULES:}${pkgs.gvfs}/lib/gio/modules"
    export XDG_DATA_DIRS="''${XDG_DATA_DIRS:-/usr/local/share:/usr/share}:${pkgs.adwaita-icon-theme}/share:${pkgs.shared-mime-info}/share:${noctalia-files.glycinLoaders}/share"
    ${pkgs.lib.optionalString pkgs.stdenv.cc.isGNU ''
      export AR="${pkgs.stdenv.cc.cc}/bin/gcc-ar"
      export RANLIB="${pkgs.stdenv.cc.cc}/bin/gcc-ranlib"
    ''}
    echo "Noctalia Files dev shell | just --list"
  '';
}
