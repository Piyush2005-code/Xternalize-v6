"""
qemu_bridge.py

TCP bridge between the MCP server (port 4444) and xv6 running in QEMU.

QEMU exposes xv6's serial console as a raw TCP stream on SERIAL_PORT (5555).
This bridge:
  1. Connects to that serial stream.
  2. Listens on BRIDGE_PORT (4444) for MCP tool calls.
  3. For each incoming connection: sends the command to xv6, waits for the
     shell prompt to come back, then returns the captured output.

Run order:
    terminal 1 → make qemu-bridge        (starts QEMU with serial on :5555)
    terminal 2 → python agent/qemu_bridge.py
    terminal 3 → python agent/mcp_client.py
"""

import os
import select
import socket
import threading
import time

# ── config (override via environment) ──────────────────────────────────────
SERIAL_HOST  = os.environ.get("XV6_SERIAL_HOST",  "127.0.0.1")
SERIAL_PORT  = int(os.environ.get("XV6_SERIAL_PORT",  "5555"))
BRIDGE_HOST  = os.environ.get("QEMU_BRIDGE_HOST", "127.0.0.1")
BRIDGE_PORT  = int(os.environ.get("QEMU_BRIDGE_PORT", "4444"))

# xv6 shell prompt — used to detect when a command has finished
XV6_PROMPT = b"$ "

# Max seconds to wait for the prompt after sending a command
CMD_TIMEOUT = 15


class SerialConnection:
    """Persistent connection to QEMU's serial TCP socket."""

    def __init__(self):
        self._sock = None
        self._lock = threading.Lock()

    def connect(self):
        print(f"[bridge] Connecting to xv6 serial at {SERIAL_HOST}:{SERIAL_PORT} …")
        while True:
            try:
                s = socket.create_connection((SERIAL_HOST, SERIAL_PORT), timeout=5)
                s.settimeout(None)          # switch to blocking after connect
                self._sock = s
                print("[bridge] Connected to xv6 serial.")
                # drain boot messages until we see the first prompt
                self._drain_until_prompt()
                return
            except (ConnectionRefusedError, OSError):
                print("[bridge] Waiting for QEMU serial …")
                time.sleep(2)

    def _drain_until_prompt(self):
        """Read and discard everything up to (and including) the first prompt."""
        buf = b""
        deadline = time.time() + 30        # give xv6 up to 30 s to boot
        while time.time() < deadline:
            ready, _, _ = select.select([self._sock], [], [], 1.0)
            if ready:
                chunk = self._sock.recv(4096)
                if not chunk:
                    raise ConnectionError("xv6 serial closed during boot")
                buf += chunk
                if XV6_PROMPT in buf:
                    return
        raise TimeoutError("xv6 did not show a prompt within 30 s")

    def run(self, command: str) -> str:
        """Send *command* to xv6 and return everything printed up to next prompt."""
        with self._lock:
            cmd_bytes = command.strip().encode() + b"\n"
            self._sock.sendall(cmd_bytes)

            buf = b""
            deadline = time.time() + CMD_TIMEOUT
            while time.time() < deadline:
                remaining = deadline - time.time()
                ready, _, _ = select.select([self._sock], [], [], min(remaining, 1.0))
                if ready:
                    chunk = self._sock.recv(4096)
                    if not chunk:
                        break
                    buf += chunk
                    if XV6_PROMPT in buf:
                        break

            return _clean_output(buf, command)


def _clean_output(raw: bytes, command: str) -> str:
    """
    Strip the echoed command and the trailing prompt from raw serial output,
    leaving only the actual command output.
    """
    text = raw.decode(errors="replace")
    # Remove the echoed command line (xv6 echoes every keystroke)
    cmd_echo = command.strip() + "\r\n"
    if text.startswith(cmd_echo):
        text = text[len(cmd_echo):]
    elif command.strip() in text:
        # crude fallback: drop up to and including the newline after the echo
        idx = text.find(command.strip())
        nl = text.find("\n", idx)
        if nl != -1:
            text = text[nl + 1:]
    # Strip trailing prompt
    if text.rstrip().endswith("$"):
        text = text[:text.rfind("$")].rstrip()
    return text.strip()


# ── bridge server ───────────────────────────────────────────────────────────

def handle_client(conn: socket.socket, addr, serial: SerialConnection):
    """Handle one MCP tool-call connection (one command per connection)."""
    try:
        data = b""
        while True:
            chunk = conn.recv(4096)
            if not chunk:
                break
            data += chunk
            if data.endswith(b"\n"):
                break

        command = data.decode(errors="replace").strip()
        if not command:
            conn.sendall(b"[bridge] empty command\n")
            return

        print(f"[bridge] cmd from {addr}: {command!r}")
        result = serial.run(command)
        conn.sendall(result.encode() + b"\n")
    except Exception as exc:
        conn.sendall(f"[bridge] error: {exc}\n".encode())
    finally:
        conn.close()


def main():
    serial = SerialConnection()
    serial.connect()

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((BRIDGE_HOST, BRIDGE_PORT))
    server.listen(8)
    print(f"[bridge] Listening for MCP tool calls on {BRIDGE_HOST}:{BRIDGE_PORT}")

    try:
        while True:
            conn, addr = server.accept()
            t = threading.Thread(target=handle_client, args=(conn, addr, serial), daemon=True)
            t.start()
    except KeyboardInterrupt:
        print("[bridge] Shutting down.")
    finally:
        server.close()


if __name__ == "__main__":
    main()
