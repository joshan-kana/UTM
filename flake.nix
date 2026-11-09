{
  description = "UTM development environment";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { nixpkgs, ... }:
    let
      systems = [ "aarch64-darwin" ];

      forDarwin = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      devShells = forDarwin (pkgs: {
        default = pkgs.mkShellNoCC {
          packages = with pkgs; [
            bison
            cmake
            gettext
            git
            glib
            gnumake
            libgpg-error
            meson
            nasm
            ninja
            nixfmt
            pkg-config
            python3
          ];
        };
      });

      packages = forDarwin (pkgs: {
        default = pkgs.utm.overrideAttrs (
          finalAttrs: previousAttrs: {
            version = "5.0.8-graphics-preview.2";

            src = pkgs.fetchurl {
              url = "https://github.com/joshan-kana/UTM/releases/download/v${finalAttrs.version}/UTM.dmg";
              hash = "sha256-xQAa5wTrIrhtwb383/qm4G8zJc7LujchqF7qTCTkVjw=";
            };

            meta = previousAttrs.meta // {
              changelog = "https://github.com/joshan-kana/UTM/releases/tag/v${finalAttrs.version}";
            };
          }
        );
      });
    };
}
