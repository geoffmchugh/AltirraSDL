{
  description = "A Nix Flake for AltirraSDL Atari 8-bit emulation software.";

  # This can and should be modified if you want to use a
  # different channel.
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    nixpkgs-stable.url = "github:NixOS/nixpkgs/nixos-26.05";
  };

  outputs = inputs@
    { self, nixpkgs, nixpkgs-stable, ... }:
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
            src = fetchFromGitHub {
              owner = "geoffmchugh";
              repo = "AltirraSDL";
              rev = "bad1e0e56593a3e65d6ac7aba9bd969323046767";
              hash = "sha256-inF9Uc8KLHZZjugqd193qLsSIL/gbQ6evd/i5xHDVQM="; # rev = "master";
            };
            doCheck = true;
            nativeBuildInputs = [
              sdl3 
              sdl3-ttf 
              sdl3-image 
              sdl3-mixer 
              sdl3-shadercross 
              cmake 
              pkg-config 
              ninja
              alsa-lib 
              jack2 
              libpulseaudio 
              pipewire 
              sndio
              wayland 
              libxkbcommon 
              libx11 
              libxext 
              libxcursor 
              libxi 
              libxfixes 
              libxrandr 
              libxscrnsaver 
              libxtst 
              libxcb 
              libGL 
              mesa
              libusb1 
              dbus 
              libdecor 
              libdrm 
              zenity 
              gcc15
            ];
            LIBRARIES = "/lib:${sdl3}/lib:${sdl3-ttf}/lib:${sdl3-image}/lib:${sdl3-mixer}/lib:${libx11}/lib:${libxext}/lib:${libxcursor}/lib:${libxi}/lib:${libxfixes}/lib:${libxrandr}/lib:${libxscrnsaver}/lib:${libxtst}/lib:${libxcb}/lib:${libGL}/lib:${libusb1}/lib:${libdecor}/lib:${libdrm}/lib";
            NIX_LDFLAGS = "-lm -lz";
            buildPhase = ''
              patchShebangs .
              # substituteInPlace makefile --replace-quiet '/sbin/ldconfig' 'ldconfig'
              # substituteInPlace makefile --replace-quiet 'grep -A 10' 'grep -A 100'
              ./build.sh
            '';
            installPhase = ''
              export 
                mkdir -p $out/bin
                cp ${imlacSimh} $out/imlac.simh
                cp BIN/imlac $out/bin
            '';
          };

          # sty = stdenv.mkDerivation {
          #   pname = "sty";
          #   version = "0.9";
          #   src = fetchgit {
          #     url = "https://github.com/obsolescence/pidp10";
          #     sha256 = "sha256-lTaprHLi90a0W6cGnGq8rNAOqVf+qmxMXYnJLjvAcwY=";
          #   };
          #   buildInputs = [
          #     libx11
          #     libxft
          #   ];
          #   nativeBuildInputs = [
          #     SDL2
          #     SDL2_image
          #     SDL2_mixer
          #     SDL2_ttf
          #     fontconfig
          #     freetype
          #     gnumake
          #     pkg-config
          #   ];
          #   patchPhase = ''
          #     # For our first order of business, the sounds
          #     # have to live somewhere, but we have no /opt.
          #     SRC=src/sty33
          #     NIX_DEST=$out/sounds

          #     # Since the WAV locations were hard-coded,
          #     # we need to update them.
          #     sed -i "s,/opt/pidp10/bin/sounds,$NIX_DEST,g" $SRC/sounds.c

          #     # And now to do some dodgy things to config.mk.
          #     sed -i 's/-lrt//' $SRC/config.mk

          #     # This seems to only affect the Mac environment.
          #     if [ ${system} == "aarch64-darwin" -o ${system} == "x86_64-darwin" ]; then
          #       sed -i 's/,--allow-shlib-undefined//' $SRC/config.mk
          #     fi

          #     sed -i -e 's/PKG_CONFIG = pkg-config//; /# Customize below to fit your system/a\' \
          #     -e 'PKG_CONFIG = pkg-config \
          #     CFLAGS := $(CFLAGS) `$(PKG_CONFIG) --cflags sdl2 SDL2_image SDL2_mixer SDL2_ttf` \
          #     LDFLAGS := $(LDFLAGS) `$(PKG_CONFIG) --libs sdl2 SDL2_image SDL2_mixer SDL2_ttf`' $SRC/config.mk
          #   '';
          #   buildPhase = ''
          #     mkdir -p $NIX_DEST

          #     cd $SRC
          #     cp sounds/*.wav $NIX_DEST

          #     make -j $NIX_BUILD_CORES
          #   '';
          #   installPhase = stdInstall;
          # };

          nix-altirrasdl = pkgs.buildEnv {
            name = "nix-altirrasdl";
            paths = [
              self.packages.${system}.imlac
              self.packages.${system}.sty
            ];
            pathsToLink = [ "/bin" ];
          };

          default = self.packages.${system}.nix-altirrasdl;
        })
      );
    };
}
