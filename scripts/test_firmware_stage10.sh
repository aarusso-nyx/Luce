#!/usr/bin/env bash
set -euo pipefail

echo "note: scripts/test_firmware_stage10.sh is retained as an alias for scripts/test_firmware_net1.sh" >&2
"$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/test_firmware_net1.sh" "$@"
