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
    };
}
