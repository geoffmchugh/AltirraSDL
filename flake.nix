{
  description = "A Nix Flake for AltirraSDL Atari 8-bit emulation software.";

  # This can and should be modified if you want to use a
  # different channel.
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    nixpkgs-stable.url = "github:NixOS/nixpkgs/nixos-26.05";
    altirrasdl-src = {
      url = "git+https://github.com/geoffmchugh/AltirraSDL";
      ref = "main";
      type = "git";
      flake = false;
    };
    imgui-src = {
      url = "git+https://github.com/ocornut/imgui";
      type = "git";
      ref = "docking";
      flake = false;
    };
  };

  outputs = inputs@
    { self, nixpkgs, nixpkgs-stable, altirrasdl-src, imgui-src, ... }:
    let
      supportedSystems = [
        "x86_64-linux"
        "aarch64-darwin"
        "x86_64-darwin"
      ];
      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
      nixpkgsFor = forAllSystems (system: import nixpkgs { inherit system; });
      nixpkgsStableFor = forAllSystems (system: import nixpkgs-stable { inherit system; });
    in
    {
      packages = forAllSystems (
        system:
        let
          pkgs = nixpkgsFor.${system};
          pkgs-stable = nixpkgsStableFor.${system};

          stdBuild = "make -j $NIX_BUILD_CORES $pname";
          stdInstall = "mkdir -p $out/bin; mv $pname $out/bin";
        in
        (with pkgs; {
          altirrasdl = stdenv.mkDerivation rec {
            pname = "altirrasdl";
            version = "1.0";
            src = altirrasdl-src;
            doCheck = true;
            nativeBuildInputs = [
              git
              sdl3 sdl3-ttf sdl3-image sdl3-mixer sdl3-shadercross 
              cmake pkg-config ninja
              alsa-lib jack2 libpulseaudio pipewire sndio
              wayland 
              libxkbcommon libx11 libxext libxcursor libxi libxfixes libxrandr libxscrnsaver libxtst libxcb libGL mesa
              libusb1 dbus 
              libdecor libdrm 
              zenity gcc15 zlib zlib.out
              pcre2       # PCRE is version 2 in modern Nix, or pcre for legacy
              libpng freetype 
              SDL2 SDL2_ttf
              vde2
              libpcap     # Pre-install to avoid downloading
              imgui
              python3
            ];
            LIBRARIES = "/lib:${sdl3}/lib:${sdl3-ttf}/lib:${sdl3-image}/lib:${sdl3-mixer}/lib:${libx11}/lib:${libxext}/lib:${libxcursor}/lib:${libxi}/lib:${libxfixes}/lib:${libxrandr}/lib:${libxscrnsaver}/lib:${libxtst}/lib:${libxcb}/lib:${libGL}/lib:${libusb1}/lib:${libdecor}/lib:${libdrm}/lib";
            NIX_LDFLAGS = "-lm -lz";
            postPatch = ''
              mkdir -p third_party/imgui
              cp -r ${imgui-src}/* third_party/imgui/
              chmod -R u+w third_party
            '';
            configurePhase = ''
              cmake . -DALTIRRA_STATIC_SDL3=OFF -DALTIRRA_ENABLE_FFMPEG_RECORDING=OFF -DALTIRRA_IMGUI_SOURCE_DIR=$PWD/third_party/imgui
            '';
            buildPhase = ''
              patchShebangs .
              # substituteInPlace makefile --replace-quiet '/sbin/ldconfig' 'ldconfig'
              # substituteInPlace makefile --replace-quiet 'grep -A 10' 'grep -A 100'
              ./build.sh --release --system-sdl3 --cmake "-DALTIRRA_STATIC_SDL3=OFF -DALTIRRA_ENABLE_FFMPEG_RECORDING=OFF -DALTIRRA_IMGUI_SOURCE_DIR=$PWD/third_party/imgui"
              # cmake --build . -j $NIX_BUILD_CORES;
            '';
            installPhase = ''
              ls -ltr $src
              ls -ltr $src/dist
              ls -ltr $src/dist/linux
              ls -ltr $src/dist/extras
              ls -ltr $src/cmake

              mkdir -p $out/bin
              mkdir -p $out/dist

              mkdir -p $out/localconfig

              cp /build/source/build/linux-release/src/AltirraSDL/AltirraSDL $out/bin
              cp -r dist/ $out/dist/
              cp -r localconfig $out/localconfig/
            '';
          };

          nix-altirrasdl = pkgs.buildEnv {
            name = "nix-altirrasdl";
            paths = [
              self.packages.${system}.altirrasdl
            ];
            pathsToLink = [ "/bin" ];
          };

          default = self.packages.${system}.nix-altirrasdl;
        })
      );
    };
}
