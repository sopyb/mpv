{ pkgs ? import <nixpkgs> { } }:

pkgs.mpv-unwrapped.overrideAttrs (old: {
  pname = "mpv";
  version = "dev";

  src = ./.; 

  postPatch = pkgs.lib.concatStringsSep "\n" [
    # Don't reference compile time dependencies or create a build outputs cycle
    # between out and dev
    ''
      substituteInPlace meson.build \
        --replace-fail "conf_data.set_quoted('CONFIGURATION', meson.build_options().strip().replace('\\\\', '\\\\\\\\'))" \
                       "conf_data.set_quoted('CONFIGURATION', '<omitted>')"
    ''
    # A trick to patchShebang everything except mpv_identify.sh
    ''
      pushd TOOLS
      mv mpv_identify.sh mpv_identify
      patchShebangs *.py *.sh
      mv mpv_identify mpv_identify.sh
      popd
    ''
  ];
})
