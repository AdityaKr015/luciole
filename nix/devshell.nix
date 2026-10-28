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
  ];

  shellHook = ''
    export NOCTALIA_FILES_ASSETS_DIR="$PWD/assets"
    echo "Noctalia Files dev shell | just --list"
  '';
}
