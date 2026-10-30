{
  description = "A native Wayland file manager using Noctalia rendering and controls.";

  inputs.nixpkgs.url = "https://channels.nixos.org/nixos-unstable/nixexprs.tar.zst";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forEachSystem = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
      withPackage =
        module:
        { pkgs, lib, ... }:
        {
          imports = [ module ];
          programs.noctalia-files.package =
            lib.mkDefault
              self.packages.${pkgs.stdenv.hostPlatform.system}.default;
        };
    in
    {
      overlays.default = final: _prev: {
        noctalia-files = final.callPackage ./nix/package.nix { };
      };

      packages = forEachSystem (pkgs: {
        default = pkgs.callPackage ./nix/package.nix { };
      });

      apps = forEachSystem (pkgs: {
        default = {
          type = "app";
          program = nixpkgs.lib.getExe self.packages.${pkgs.stdenv.hostPlatform.system}.default;
          meta.description = self.packages.${pkgs.stdenv.hostPlatform.system}.default.meta.description;
        };
      });

      devShells = forEachSystem (pkgs: {
        default = pkgs.callPackage ./nix/devshell.nix {
          noctalia-files = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
        };
      });

      checks = forEachSystem (pkgs: {
        package = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
        module-config = import ./nix/module-checks.nix {
          inherit pkgs;
          package = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
        };
        headless-cli = self.packages.${pkgs.stdenv.hostPlatform.system}.default.overrideAttrs (_: {
          mesonFlags = [ "-Dtests=enabled" ];
          doCheck = true;
          preCheck = ''
            export XDG_DATA_DIRS="${pkgs.shared-mime-info}/share"
          '';
        });
      });

      formatter = forEachSystem (pkgs: pkgs.nixfmt);
      homeModules.default = withPackage ./nix/home-module.nix;
      hjemModules.default = withPackage ./nix/hjem-module.nix;
      nixosModules.default = withPackage ./nix/nixos-module.nix;
    };
}
