#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

BPT_HEADER="src/include/storage/index/b_plus_tree.h"
BPT_SOURCE="src/storage/index/b_plus_tree.cpp"
TEST_SCRIPT="build_support/run-bpt-tests.sh"

usage() {
  cat <<EOF
Usage:
  ./run-student-bpt-tests.sh [newdir] [-- run-bpt-tests options]

Examples:
  ./run-student-bpt-tests.sh /path/to/newdir
  ./run-student-bpt-tests.sh /path/to/newdir -- --clean

If newdir is omitted, this script will prompt for it.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

NEW_DIR="${1:-}"
if [[ -z "$NEW_DIR" ]]; then
  read -r -p "Paste newdir: " NEW_DIR
fi

if [[ -z "$NEW_DIR" ]]; then
  echo "newdir cannot be empty." >&2
  exit 2
fi

shift_count=0
if [[ $# -gt 0 ]]; then
  shift_count=1
fi

if [[ "$shift_count" -eq 1 ]]; then
  shift
fi

if [[ "${1:-}" == "--" ]]; then
  shift
fi

case "$NEW_DIR" in
  \"*\") NEW_DIR="${NEW_DIR#\"}"; NEW_DIR="${NEW_DIR%\"}" ;;
  \'*\') NEW_DIR="${NEW_DIR#\'}"; NEW_DIR="${NEW_DIR%\'}" ;;
esac

NEW_DIR="${NEW_DIR%/}"
SUBMISSION_ROOT="$NEW_DIR/BPlusTree"

if [[ ! -d "$SUBMISSION_ROOT" ]]; then
  echo "Could not find submission root: $SUBMISSION_ROOT" >&2
  echo "Pass the directory that contains BPlusTree as newdir." >&2
  exit 1
fi

for rel_path in "$BPT_HEADER" "$BPT_SOURCE"; do
  src="$SUBMISSION_ROOT/$rel_path"
  dst="$ROOT_DIR/$rel_path"

  if [[ ! -f "$src" ]]; then
    echo "Missing source file: $src" >&2
    exit 1
  fi

  if [[ ! -f "$dst" ]]; then
    echo "Missing destination file: $dst" >&2
    exit 1
  fi
done

echo "Copying B+ tree files from: $SUBMISSION_ROOT"
cp "$SUBMISSION_ROOT/$BPT_HEADER" "$ROOT_DIR/$BPT_HEADER"
cp "$SUBMISSION_ROOT/$BPT_SOURCE" "$ROOT_DIR/$BPT_SOURCE"

echo "Running B+ tree tests..."
cd "$ROOT_DIR"
bash "$TEST_SCRIPT" "$@"
