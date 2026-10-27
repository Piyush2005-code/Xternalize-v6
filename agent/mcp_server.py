"""
mcp_server.py

Minimal MCP (Model Context Protocol) server that exposes xv6 tools.
Each tool sends a command through stdin/stdout to a QEMU bridge process
(launched separately).  For now the bridge is a placeholder socket/pipe
that will be wired in the next commit.

Run directly:
    python agent/mcp_server.py
"""

import json
import sys
import subprocess
import socket
import os

# ---------------------------------------------------------------------------
# QEMU bridge config  (will be populated when the bridge is implemented)
# ---------------------------------------------------------------------------
BRIDGE_HOST = os.environ.get("QEMU_BRIDGE_HOST", "127.0.0.1")
BRIDGE_PORT = int(os.environ.get("QEMU_BRIDGE_PORT", "4444"))


def _send_to_xv6(command: str) -> str:
    """
    Send a shell command to the xv6 instance via the QEMU bridge socket.
    The bridge sends the command output followed by a newline, then closes
    the connection — so we read until EOF.
    """
    try:
        with socket.create_connection((BRIDGE_HOST, BRIDGE_PORT), timeout=20) as sock:
            sock.sendall((command.strip() + "\n").encode())
            chunks = []
            while True:
                data = sock.recv(4096)
                if not data:
                    break
                chunks.append(data.decode(errors="replace"))
            return "".join(chunks).strip()
    except ConnectionRefusedError:
        return (
            f"[bridge] Connection refused at {BRIDGE_HOST}:{BRIDGE_PORT}. "
            "Start the bridge first: python agent/qemu_bridge.py"
        )
    except Exception as exc:
        return f"[bridge] Error: {exc}"


# ---------------------------------------------------------------------------
# Tool implementations
# ---------------------------------------------------------------------------

def tool_run_command(command: str) -> str:
    """Run an arbitrary shell command inside xv6."""
    return _send_to_xv6(command)


def tool_list_files(path: str = "/") -> str:
    """List files in a directory inside xv6 (calls `ls <path>`)."""
    return _send_to_xv6(f"ls {path}")


def tool_read_file(path: str) -> str:
    """Read a file inside xv6 (calls `cat <path>`)."""
    return _send_to_xv6(f"cat {path}")


def tool_write_file(path: str, content: str) -> str:
    """
    Write content to a file inside xv6 using echo redirect.
    Note: xv6 echo does not support escape sequences — keep content simple.
    """
    return _send_to_xv6(f"echo {content} > {path}")


def tool_get_processes() -> str:
    """List running processes inside xv6 (no `ps`; uses a custom proc dump if available)."""
    return _send_to_xv6("cat /proc/procs 2>/dev/null || echo 'proc list not available'")


# ---------------------------------------------------------------------------
# Tool registry
# ---------------------------------------------------------------------------

TOOLS = {
    "run_command": tool_run_command,
    "list_files": tool_list_files,
    "read_file": tool_read_file,
    "write_file": tool_write_file,
    "get_processes": tool_get_processes,
}

# JSON schemas consumed by the LLM agent
TOOL_DEFINITIONS = [
    {
        "name": "run_command",
        "description": "Run an arbitrary command inside the xv6 OS via the QEMU bridge.",
        "parameters": {
            "type": "object",
            "properties": {
                "command": {"type": "string", "description": "Shell command to run in xv6."},
            },
            "required": ["command"],
        },
    },
    {
        "name": "list_files",
        "description": "List files in a directory inside xv6.",
        "parameters": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Directory path to list.", "default": "/"},
            },
            "required": [],
        },
    },
    {
        "name": "read_file",
        "description": "Read the contents of a file inside xv6.",
        "parameters": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "File path to read."},
            },
            "required": ["path"],
        },
    },
    {
        "name": "write_file",
        "description": "Write content to a file inside xv6.",
        "parameters": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Destination file path."},
                "content": {"type": "string", "description": "Text to write into the file."},
            },
            "required": ["path", "content"],
        },
    },
    {
        "name": "get_processes",
        "description": "List running processes in the xv6 OS.",
        "parameters": {"type": "object", "properties": {}, "required": []},
    },
]


# ---------------------------------------------------------------------------
# MCP stdio transport  (JSON-RPC 2.0 over stdin/stdout)
# ---------------------------------------------------------------------------

def _respond(req_id, result=None, error=None):
    obj = {"jsonrpc": "2.0", "id": req_id}
    if error:
        obj["error"] = {"code": -32000, "message": str(error)}
    else:
        obj["result"] = result
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def _handle(request: dict):
    method = request.get("method", "")
    req_id = request.get("id")
    params = request.get("params", {})

    if method == "tools/list":
        _respond(req_id, {"tools": TOOL_DEFINITIONS})

    elif method == "tools/call":
        name = params.get("name")
        args = params.get("arguments", {})
        fn = TOOLS.get(name)
        if fn is None:
            _respond(req_id, error=f"Unknown tool: {name}")
            return
        try:
            result = fn(**args)
            _respond(req_id, {"content": [{"type": "text", "text": str(result)}]})
        except Exception as exc:
            _respond(req_id, error=str(exc))

    elif method == "initialize":
        _respond(req_id, {
            "protocolVersion": "2024-11-05",
            "serverInfo": {"name": "xv6-mcp-server", "version": "0.1.0"},
            "capabilities": {"tools": {}},
        })

    else:
        _respond(req_id, error=f"Method not found: {method}")


def main():
    print("[mcp_server] xv6 MCP server started (stdio transport)", file=sys.stderr)
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            request = json.loads(line)
        except json.JSONDecodeError as exc:
            sys.stdout.write(json.dumps({
                "jsonrpc": "2.0", "id": None,
                "error": {"code": -32700, "message": f"Parse error: {exc}"},
            }) + "\n")
            sys.stdout.flush()
            continue
        _handle(request)


if __name__ == "__main__":
    main()
