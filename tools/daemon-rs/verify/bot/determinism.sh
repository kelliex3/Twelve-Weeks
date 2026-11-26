#!/bin/bash
# determinism.sh <python|rust> <outprefix> [scenario ...]
# Runs the scenarios twice with the same seed (outputs <outprefix>.1 and <outprefix>.2) and diffs the transcripts: every scenario must be SAME.
KIND=$1; PFX=$2; shift 2
HERE=$(cd "$(dirname "$0")" && pwd)
"$HERE/run_all.sh" "$KIND" "$PFX.1" "$@"
"$HERE/run_all.sh" "$KIND" "$PFX.2" "$@"
"$HERE/diff.sh" "$PFX.1" "$PFX.2" "$@"
