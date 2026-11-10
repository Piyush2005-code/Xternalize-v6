#!/usr/bin/env bash
#
# demo.sh — Run the official demo script for speculative execution.
#
# Usage:
#   ./scripts/demo.sh [--cpus 1] [--mode all]
#

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

python3 "$ROOT/scripts/demo_spec.py" "$@"
