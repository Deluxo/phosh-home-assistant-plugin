{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  packages = with pkgs; [
    gnumake
    openssh
    gnutar
    gzip
    git
    python3
  ];

  shellHook = ''
    echo "dev shell: make + ssh + python3 (see Makefile targets)"
  '';
}
