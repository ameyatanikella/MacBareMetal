#!/bin/sh
# Install the build toolchain on macOS (Apple Silicon, Homebrew).
#
# Verified on macOS 26.6.2 / Mac17,9 on 2026-11-09:
#   llvm + lld 23.1.4, GNU make 4.4.1, rustc 1.99.1.
# m1n1's README asks for `brew install llvm lld` and the Rust target below.
# GNU make 4 is our addition: m1n1's Makefile uses grouped targets (&:), which
# the make 3.81 that ships with macOS does not understand.
set -eu

command -v brew >/dev/null || { echo "Homebrew is required: https://brew.sh"; exit 1; }

brew install llvm lld make rustup

RUSTUP="$(brew --prefix rustup)/bin/rustup"
"$RUSTUP" toolchain install stable --profile minimal \
    --target aarch64-unknown-none-softfloat --no-self-update
"$RUSTUP" default stable

echo
echo "Toolchain ready. The Makefile adds rustup and GNU make to PATH itself;"
echo "nothing was added to your shell profile."
