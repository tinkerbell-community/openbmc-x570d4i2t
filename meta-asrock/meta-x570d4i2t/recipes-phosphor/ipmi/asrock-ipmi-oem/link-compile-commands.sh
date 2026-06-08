#!/usr/bin/env bash
# Link bitbake's generated compile_commands.json into the recipe source dir
# so clangd can find it when opening files in this directory.
#
# Usage: ./link-compile-commands.sh
# Prerequisite: bitbake -c configure asrock-ipmi-oem

set -euo pipefail

RECIPE_DIR="$(cd "$(dirname "$0")" && pwd)"
OPENBMC_ROOT="$(realpath "$RECIPE_DIR/../../../../..")"
WORK_DIR="$OPENBMC_ROOT/tmp/work"

if [ ! -d "$WORK_DIR" ]; then
    echo "ERROR: $WORK_DIR not found — have you sourced oe-init-build-env?" >&2
    exit 1
fi

DB="$(find "$WORK_DIR" -name "compile_commands.json" \
        -path "*asrock-ipmi-oem*" 2>/dev/null | head -1)"

if [ -z "$DB" ]; then
    echo "compile_commands.json not found. Run first:" >&2
    echo "  bitbake -c configure asrock-ipmi-oem" >&2
    exit 1
fi

ln -sf "$DB" "$RECIPE_DIR/compile_commands.json"
echo "Linked: $DB"
echo "       → $RECIPE_DIR/compile_commands.json"
