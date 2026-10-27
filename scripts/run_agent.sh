#!/usr/bin/env bash
#
# run_agent.sh — start xv6 (QEMU), the serial bridge and the AI agent with
# one command, and shut everything down when the agent exits.
#
#   scripts/run_agent.sh
#
# Logs for the background pieces go to scripts/logs/{qemu,bridge}.log.
# Environment variables understood by the agent (XV6_SERIAL_PORT,
# QEMU_BRIDGE_PORT, XV6_USER, XV6_PASSWORD, OPENROUTER_API_KEY …) are passed
# through unchanged.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

LOG_DIR="$ROOT/scripts/logs"
QEMU_LOG="$LOG_DIR/qemu.log"
BRIDGE_LOG="$LOG_DIR/bridge.log"
mkdir -p "$LOG_DIR"

export XV6_SERIAL_PORT="${XV6_SERIAL_PORT:-5555}"
export QEMU_BRIDGE_PORT="${QEMU_BRIDGE_PORT:-4444}"
BRIDGE_TIMEOUT="${BRIDGE_TIMEOUT:-90}"   # seconds to wait for xv6 boot + login

# ── pick a Python: the project venv if present, else system python3 ───────
if [[ -x "$ROOT/project_env/bin/python" ]]; then
  PY="$ROOT/project_env/bin/python"
else
  PY="$(command -v python3)"
fi

info() { printf '\033[1;36m[run_agent]\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m[run_agent] %s\033[0m\n' "$*" >&2; exit 1; }

port_in_use() { (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }

# ── pre-flight checks ──────────────────────────────────────────────────────
if [[ -z "${OPENROUTER_API_KEY:-}" ]] &&
   ! grep -qs '^[[:space:]]*\(export[[:space:]]\+\)\?OPENROUTER_API_KEY=' agent/.env .env; then
  fail "OPENROUTER_API_KEY is not set (export it or put it in agent/.env)."
fi

"$PY" -c 'import requests, dotenv' 2>/dev/null ||
  fail "Missing Python packages. Run: $PY -m pip install -r agent/requirements.txt"

for p in "$XV6_SERIAL_PORT" "$QEMU_BRIDGE_PORT"; do
  if port_in_use "$p"; then
    fail "Port $p is already in use — an old QEMU or bridge is probably still running. Stop it first."
  fi
done

# ── cleanup on exit (Ctrl+C, agent quits, or an error) ─────────────────────
QEMU_PID=""
BRIDGE_PID=""
cleanup() {
  trap - EXIT INT TERM
  echo
  info "Shutting down …"
  [[ -n "$BRIDGE_PID" ]] && kill "$BRIDGE_PID" 2>/dev/null || true
  # QEMU runs in its own process group (setsid), so kill the whole group:
  # that takes out both `make` and the qemu-system-riscv64 child.
  [[ -n "$QEMU_PID" ]] && kill -- -"$QEMU_PID" 2>/dev/null || true
  # Belt and braces, in case QEMU ended up outside that group.
  [[ -n "$QEMU_PID" ]] && pkill -f "qemu-system-riscv64.*tcp::${XV6_SERIAL_PORT}," 2>/dev/null || true
  wait 2>/dev/null || true
  info "Done."
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# ── 1. build ───────────────────────────────────────────────────────────────
info "Building xv6 …"
make kernel/kernel fs.img >/dev/null || fail "Build failed — run 'make' to see the errors."

# ── 2. QEMU with serial on :$XV6_SERIAL_PORT ───────────────────────────────
info "Starting QEMU (serial on :$XV6_SERIAL_PORT, log: scripts/logs/qemu.log) …"
setsid make qemu-bridge XV6_SERIAL_PORT="$XV6_SERIAL_PORT" >"$QEMU_LOG" 2>&1 < /dev/null &
QEMU_PID=$!

for _ in $(seq 1 20); do
  port_in_use "$XV6_SERIAL_PORT" && break
  kill -0 "$QEMU_PID" 2>/dev/null || { cat "$QEMU_LOG" >&2; fail "QEMU exited early (see above)."; }
  sleep 0.5
done
port_in_use "$XV6_SERIAL_PORT" || fail "QEMU did not open port $XV6_SERIAL_PORT. See $QEMU_LOG"

# ── 3. bridge on :$QEMU_BRIDGE_PORT ────────────────────────────────────────
info "Starting bridge (log: scripts/logs/bridge.log), waiting for xv6 to boot and log in …"
"$PY" -u agent/qemu_bridge.py >"$BRIDGE_LOG" 2>&1 < /dev/null &
BRIDGE_PID=$!

deadline=$((SECONDS + BRIDGE_TIMEOUT))
until grep -q "Listening for MCP tool calls" "$BRIDGE_LOG"; do
  if ! kill -0 "$BRIDGE_PID" 2>/dev/null; then
    cat "$BRIDGE_LOG" >&2
    fail "Bridge exited (see above)."
  fi
  if (( SECONDS >= deadline )); then
    cat "$BRIDGE_LOG" >&2
    fail "Bridge was not ready after ${BRIDGE_TIMEOUT}s."
  fi
  sleep 0.5
done
info "$(grep -m1 'Logged in' "$BRIDGE_LOG" | sed 's/^\[bridge\] //')"

# ── 4. agent REPL (foreground) ─────────────────────────────────────────────
info "Starting agent — type 'exit' or press Ctrl+D to quit."
echo
set +e
"$PY" agent/mcp_client.py
