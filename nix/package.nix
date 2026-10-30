{
  lib,
  stdenv,
  meson,
  ninja,
  pkg-config,
  python3,
  wayland-scanner,
  wayland,
  wayland-protocols,
  libglvnd,
  libxkbcommon,
  freetype,
  fontconfig,
  cairo,
  glib,
  gvfs,
  pango,
  harfbuzz,
  libpng,
  libwebp,
  librsvg,
  gdk-pixbuf,
  ffmpeg,
  poppler-utils,
  nlohmann_json,
  tomlplusplus,
  autoAddDriverRunpath,
  makeWrapper,
  adwaita-icon-theme,
  shared-mime-info,
  xdg-terminal-exec,
  buildRevision ? "unknown",
}:
stdenv.mkDerivation (finalAttrs: {
  pname = "noctalia-files";
  version = lib.fileContents ../VERSION;

  src = lib.cleanSourceWith {
    src = ./..;
    filter =
      path: type:
      let
        name = baseNameOf path;
      in
      lib.cleanSourceFilter path type
      && !(builtins.elem name [
        ".agents"
        ".aws"
        ".codex"
        ".cache"
        ".direnv"
        ".envrc"
        ".idea"
        ".vscode"
        "__pycache__"
        "result"
        "AGENTS.md"
        "PLAN.md"
      ])
      && !(lib.hasPrefix "build-" name)
      && !(lib.hasPrefix "result-" name);
  };

  nativeBuildInputs = [
    meson
    ninja
    pkg-config
    python3
    wayland-scanner
    autoAddDriverRunpath
    makeWrapper
  ];

  buildInputs = [
    wayland
    wayland-protocols
    libglvnd
    libxkbcommon
    freetype
    fontconfig
    cairo
    glib
    pango
    harfbuzz
    libpng
    libwebp
    librsvg
    gdk-pixbuf
    nlohmann_json
    tomlplusplus
  ];

  mesonBuildType = "release";
  postFixup = ''
    wrapProgram "$out/bin/noctalia-files" \
      --prefix PATH : "${
        lib.makeBinPath [
          xdg-terminal-exec
          ffmpeg
          poppler-utils
        ]
      }" \
      --suffix GIO_EXTRA_MODULES : "${gvfs}/lib/gio/modules" \
      --suffix XDG_DATA_DIRS : "${
        lib.makeSearchPath "share" [
          adwaita-icon-theme
          shared-mime-info
        ]
      }"
  '';
  mesonFlags = [
    "-Dtests=disabled"
    (lib.mesonOption "build_revision" buildRevision)
  ];

  meta = {
    description = "A native Wayland file manager using Noctalia rendering and controls (in development)";
    homepage = "https://github.com/noctalia-dev/noctalia-files";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
    mainProgram = finalAttrs.pname;
  };
})
