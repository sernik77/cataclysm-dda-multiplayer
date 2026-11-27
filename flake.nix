{
  description = "Cataclysm: Dark Days Ahead with experimental co-op multiplayer";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      makeFlags = [
        "USE_XDG_DIR=1"
        "RELEASE=1"
        "LTO=0"
        "LOCALIZE=0"
        "BACKTRACE=0"
        "RUNTESTS=0"
        "LINTJSON=0"
        "ASTYLE=0"
        "PCH=0"
      ];

      mkCdda = pkgs: { tiles }:
        pkgs.stdenv.mkDerivation {
          pname = if tiles then "cataclysm-dda-mp-tiles" else "cataclysm-dda-mp";
          version = "0-unstable-${self.shortRev or self.dirtyShortRev or "dirty"}";
          src = self;

          nativeBuildInputs = [ pkgs.pkg-config ];
          buildInputs = [ pkgs.ncurses pkgs.zlib pkgs.python3 ] ++ pkgs.lib.optionals tiles [
            pkgs.SDL2 pkgs.SDL2_image pkgs.SDL2_ttf pkgs.SDL2_mixer pkgs.freetype
          ];

          enableParallelBuilding = true;
          # There is no .git in the Nix source, so name the version here.
          makeFlags = makeFlags ++ [ "PREFIX=$(out)" "VERSION=${self.shortRev or "dirty"}-mp" ]
            ++ (if tiles then [ "TILES=1" "SOUND=1" ] else [ "TILES=0" "SOUND=0" ]);

          postInstall = ''
            install -Dm755 tools/mp/mp_probe.py $out/bin/cataclysm-mp-probe
            patchShebangs $out/bin/cataclysm-mp-probe
            install -Dm644 packaging/completions/cataclysm.bash \
              $out/share/bash-completion/completions/cataclysm
            ln -s cataclysm $out/share/bash-completion/completions/cataclysm-tiles
            install -Dm644 packaging/completions/_cataclysm \
              $out/share/zsh/site-functions/_cataclysm
            install -Dm644 packaging/completions/cataclysm.fish \
              $out/share/fish/vendor_completions.d/cataclysm.fish
            install -Dm644 doc/c++/MULTIPLAYER.md $out/share/doc/cataclysm-dda-mp/MULTIPLAYER.md
            install -Dm644 LICENSE.txt $out/share/licenses/cataclysm-dda-mp/LICENSE.txt
          '';

          meta = {
            description = "Cataclysm: Dark Days Ahead with experimental co-op multiplayer";
            homepage = "https://github.com/sernik77/cataclysm-dda-multiplayer";
            license = pkgs.lib.licenses.cc-by-sa-30;
            platforms = systems;
            mainProgram = if tiles then "cataclysm-tiles" else "cataclysm";
          };
        };
    in
    {
      packages = forAll (pkgs: rec {
        tiles = mkCdda pkgs { tiles = true; };
        curses = mkCdda pkgs { tiles = false; };
        default = tiles;
      });

      apps = forAll (pkgs:
        let p = self.packages.${pkgs.stdenv.hostPlatform.system}; in {
          default = { type = "app"; program = "${p.tiles}/bin/cataclysm-tiles"; };
          curses = { type = "app"; program = "${p.curses}/bin/cataclysm"; };
          probe = { type = "app"; program = "${p.curses}/bin/cataclysm-mp-probe"; };
        });

      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.tiles ];
          packages = [ pkgs.astyle pkgs.ccache pkgs.gdb pkgs.python3 ];
        };
      });
    };
}
