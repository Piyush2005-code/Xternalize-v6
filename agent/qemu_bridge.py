"""
qemu_bridge.py

TCP bridge between the MCP server (port 4444) and xv6 running in QEMU.

QEMU exposes xv6's serial console as a raw TCP stream on SERIAL_PORT (5555).
This bridge:
  1. Connects to that serial stream.
  2. Gets xv6 to a shell prompt — logging in through `login` if needed.
  3. Listens on BRIDGE_PORT (4444) for MCP tool calls.
  4. For each incoming connection: sends the command to xv6, waits for the
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

# Credentials used to get past user/login.c ("admin" needs a password,
# "user" does not).
XV6_USER     = os.environ.get("XV6_USER", "admin")
XV6_PASSWORD = os.environ.get("XV6_PASSWORD", "admin")

# Prompts printed by user/sh.c (getcmd) and user/login.c.
SHELL_PROMPTS = (b"admin# ", b"user$ ", b"$ ")
LOGIN_PROMPT  = b"Login as (admin/user): "
PASS_PROMPT   = b"Password: "

# Max seconds to wait for the prompt after sending a command
CMD_TIMEOUT  = 15
# Max seconds to wait for xv6 to boot and reach a shell
BOOT_TIMEOUT = 60


class BridgeSetupError(Exception):
    """xv6 is reachable but we could not get it to a shell prompt."""


def _ends_with_shell_prompt(buf: bytes) -> bool:
    return buf.endswith(SHELL_PROMPTS)


class SerialConnection:
    """Persistent connection to QEMU's serial TCP socket."""

    def __init__(self):
        self._sock = None
        self._lock = threading.Lock()

    # ── connection / login ────────────────────────────────────────────────

    def connect(self):
        print(f"[bridge] Connecting to xv6 serial at {SERIAL_HOST}:{SERIAL_PORT} …")
        while True:
            try:
                s = socket.create_connection((SERIAL_HOST, SERIAL_PORT), timeout=5)
            except OSError:
                print("[bridge] Waiting for QEMU serial (is `make qemu-bridge` running?) …")
                time.sleep(2)
                continue
            s.settimeout(None)              # switch to blocking after connect
            self._sock = s
            print("[bridge] Connected to xv6 serial.")
            break

        self._reach_shell(BOOT_TIMEOUT)
        print(f"[bridge] Logged in to xv6 as '{XV6_USER}', shell is ready.")

    def _recv(self, wait: float) -> bytes:
        ready, _, _ = select.select([self._sock], [], [], wait)
        if not ready:
            return b""
        chunk = self._sock.recv(4096)
        if not chunk:
            raise ConnectionError("xv6 serial connection closed (did QEMU exit?)")
        return chunk

    def _reach_shell(self, timeout: float) -> bytes:
        """
        Drive xv6 to a shell prompt.

        QEMU's `nowait` serial drops whatever xv6 printed before we connected,
        so we can't rely on seeing the boot prompt — we press Enter to make
        xv6 print a fresh one (login banner or shell prompt), then answer the
        login / password questions until we land at a shell.
        """
        buf = b""
        deadline = time.time() + timeout
        last_nudge = 0.0
        while time.time() < deadline:
            # Nudge xv6 with a newline every 3 s until it shows any prompt.
            if not buf.strip() and time.time() - last_nudge > 3:
                self._sock.sendall(b"\n")
                last_nudge = time.time()

            buf += self._recv(0.5)

            if _ends_with_shell_prompt(buf):
                return buf
            if buf.endswith(LOGIN_PROMPT):
                self._sock.sendall(XV6_USER.encode() + b"\n")
                buf = b""
                last_nudge = time.time()
            elif buf.endswith(PASS_PROMPT):
                self._sock.sendall(XV6_PASSWORD.encode() + b"\n")
                buf = b""
                last_nudge = time.time()
            elif b"Login incorrect" in buf or b"Unknown user" in buf:
                raise BridgeSetupError(
                    f"xv6 rejected login as '{XV6_USER}'. "
                    "Set XV6_USER / XV6_PASSWORD to valid credentials."
                )

        tail = buf.decode(errors="replace")[-200:]
        raise BridgeSetupError(
            f"xv6 did not reach a shell prompt within {timeout:.0f} s. "
            f"Last output: {tail!r}"
        )

    # ── commands ──────────────────────────────────────────────────────────

    def run(self, command: str) -> str:
        """Send *command* to xv6 and return everything printed up to next prompt."""
        with self._lock:
            cmd_bytes = command.strip().encode() + b"\n"
            self._sock.sendall(cmd_bytes)

            buf = b""
            deadline = time.time() + CMD_TIMEOUT
            while time.time() < deadline:
                buf += self._recv(min(deadline - time.time(), 1.0))
                if _ends_with_shell_prompt(buf):
                    break
                if buf.endswith(LOGIN_PROMPT):
                    # The shell exited (e.g. the command was `exit`) and init
                    # restarted login. Log back in so later commands still work.
                    self._sock.sendall(XV6_USER.encode() + b"\n")
                    self._reach_shell(CMD_TIMEOUT)
                    return (_clean_output(buf, command) +
                            "\n[bridge] shell exited; logged back in.").strip()
            else:
                return (_clean_output(buf, command) +
                        f"\n[bridge] timed out after {CMD_TIMEOUT}s waiting for the prompt.").strip()

            return _clean_output(buf, command)


def _clean_output(raw: bytes, command: str) -> str:
    """
    Strip the echoed command and the trailing prompt from raw serial output,
    leaving only the actual command output.
    """
    text = raw.decode(errors="replace").replace("\r\n", "\n")
    # Remove the echoed command line (xv6 echoes every keystroke)
    cmd = command.strip()
    if text.startswith(cmd + "\n"):
        text = text[len(cmd) + 1:]
    elif cmd in text:
        idx = text.find(cmd)
        nl = text.find("\n", idx)
        if nl != -1:
            text = text[nl + 1:]
    # Strip trailing prompt / login banner
    for p in SHELL_PROMPTS + (LOGIN_PROMPT,):
        p = p.decode()
        if text.endswith(p):
            text = text[: -len(p)]
            break
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
    try:
        serial.connect()
    except (BridgeSetupError, ConnectionError) as exc:
        print(f"[bridge] ERROR: {exc}")
        raise SystemExit(1)

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
