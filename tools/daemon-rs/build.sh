#!/bin/sh
set -e
cd "$(dirname "$0")"
export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-$HOME/.cache/t2r-daemon-target}"
cargo build --release --target x86_64-unknown-linux-musl "$@"
cp "$CARGO_TARGET_DIR/x86_64-unknown-linux-musl/release/t2r-daemon" ./t2r-daemon.bin 2>/dev/null || true
ls -la "$CARGO_TARGET_DIR/x86_64-unknown-linux-musl/release/t2r-daemon"
