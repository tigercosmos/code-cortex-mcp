{
  description = "codebase-memory-mcp — C11 MCP server for codebase indexing";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAllSystems (pkgs: {
        default = pkgs.stdenv.mkDerivation {
          pname = "codebase-memory-mcp";
          version = "0.6.0";

          src = ./.;

          nativeBuildInputs = [ pkgs.cmake ];
          buildInputs = [ pkgs.zlib ];

          # Build the production binary directly with CMake (-O2, no NDEBUG so the
          # vendored tree-sitter scanners keep their assertions; WERROR off for the
          # not-yet-clean C++ bodies). Bypasses scripts/build.sh, whose `file`-based
          # compiler check fails on Nix's bash-wrapper CC; the Nix stdenv already
          # guarantees the right compiler and target.
          dontUseCmakeConfigure = true;
          buildPhase = ''
            cmake -S . -B build/c -DCBM_SANITIZE=OFF -DCBM_WERROR=OFF \
              -DCMAKE_BUILD_TYPE=None -DCMAKE_C_FLAGS=-O2 -DCMAKE_CXX_FLAGS=-O2
            cmake --build build/c -j$NIX_BUILD_CORES --target codebase-memory-mcp
          '';

          installPhase = ''
            install -Dm755 build/c/codebase-memory-mcp $out/bin/codebase-memory-mcp
          '';

          meta = {
            description = "MCP server that builds and queries a semantic graph of your codebase";
            homepage = "https://github.com/tigercosmos/code-cortex-mcp";
            license = nixpkgs.lib.licenses.mit;
            mainProgram = "codebase-memory-mcp";
            platforms = systems;
          };
        };
      });

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.system}.default ];
          # libgit2 is an optional dependency auto-detected via pkg-config at
          # build time. When present it accelerates git history parsing;
          # otherwise the build falls back to shelling out to `git log`.
          packages = [ pkgs.pkg-config pkgs.libgit2 ];
        };
      });
    };
}
