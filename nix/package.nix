{
  lib,
  stdenv,
  meson,
  ninja,
  pkg-config,
  python3,
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
  ];

  mesonBuildType = "release";
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
