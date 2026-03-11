{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
      in
      {
        devShell = pkgs.mkShell {
          buildInputs = with pkgs; [
            qt6.qtwayland
            kdePackages.qtkeychain
            ninja
            cmake
            pkg-config
            openssl.dev
            libopus.dev
            pipewire.dev
            ffmpeg-full
            nv-codec-headers-12
            nlohmann_json
            pipewire
            libnotify
            shaderc
            gdb
         ];
        };
      }
    );
}