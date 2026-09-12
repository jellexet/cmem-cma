#!/usr/bin/env bash

# Setup script to enable pre-commit hooks. Just run it one time after cloning

set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

chmod +x .hooks/* 2>/dev/null || true
git config core.hooksPath .hooks

echo "core.hooksPath set to .hooks — this local clone's git hooks are now active."

git config blame.ignoreRevsFile .git-blame-ignore-revs

echo "blame.ignoreRevsFile set to .git-blame-ignore-revs"
