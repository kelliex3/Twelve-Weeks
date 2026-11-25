#!/bin/sh
set -e
cd "$(dirname "$0")"
export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-$HOME/.cache/t2r-daemon-target}"
cargo build --release --target x86_64-unknown-linux-musl "$@"
ls -la "$CARGO_TARGET_DIR/x86_64-unknown-linux-musl/release/" | grep t2r-
