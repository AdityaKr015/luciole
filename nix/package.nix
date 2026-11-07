{
  lib,
  stdenv,
  fetchurl,
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
  libarchive,
  acl,
  libwebp,
  librsvg,
  gdk-pixbuf,
  libglycin,
  glycin-loaders,
  stb,
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
let
  pixbufWithGlycin = gdk-pixbuf.overrideAttrs (previous: {
    # 2.44.8 requires Glycin 2.2; the pinned collection provides its 2.1 API.
    version = "2.44.6";
    src = fetchurl {
      url = "https://download.gnome.org/sources/gdk-pixbuf/2.44/gdk-pixbuf-2.44.6.tar.xz";
      hash = "sha256-FAwtC4mfz4U+6SsmNzydwijbzeCCCkJGaT9DKKJ0Zvo=";
    };
    buildInputs = (previous.buildInputs or [ ]) ++ [
      libglycin
      glycin-loaders
      shared-mime-info
    ];
    preBuild = (previous.preBuild or "") + ''
      export XDG_DATA_DIRS="${glycin-loaders}/share:${shared-mime-info}/share:''${XDG_DATA_DIRS:-}"
    '';
    mesonFlags =
      builtins.filter (
        flag:
        !(lib.any (name: lib.hasPrefix "-D${name}=" flag) [
          "glycin"
          "builtin_loaders"
          "png"
          "jpeg"
          "gif"
          "tiff"
          "others"
          "legacy_xpm"
        ])
      ) previous.mesonFlags
      ++ [
        "-Dglycin=enabled"
        "-Dbuiltin_loaders=glycin"
        "-Dpng=disabled"
        "-Djpeg=disabled"
        "-Dgif=disabled"
        "-Dtiff=disabled"
        "-Dothers=disabled"
        "-Dlegacy_xpm=disabled"
      ];
  });
  svgWithGlycin = librsvg.override { gdk-pixbuf = pixbufWithGlycin; };
in
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
    libarchive
    acl
    libwebp
    pixbufWithGlycin
    svgWithGlycin
    libglycin
    glycin-loaders
    stb
    nlohmann_json
    tomlplusplus
  ];

  # GNU LTO archives need the compiler's plugin-aware tools.
  preConfigure = lib.optionalString stdenv.cc.isGNU ''
    export AR="${stdenv.cc.cc}/bin/gcc-ar"
    export RANLIB="${stdenv.cc.cc}/bin/gcc-ranlib"
  '';

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
          glycin-loaders
        ]
      }"
  '';
  mesonFlags = [
    "-Dtests=disabled"
    (lib.mesonOption "build_revision" buildRevision)
  ];
  passthru.glycinLoaders = glycin-loaders;

  meta = {
    description = "A native Wayland file manager using Noctalia rendering and controls (in development)";
    homepage = "https://github.com/noctalia-dev/noctalia-files";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
    mainProgram = finalAttrs.pname;
  };
})
