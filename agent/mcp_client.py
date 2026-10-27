"""
mcp_client.py

Launches mcp_server.py as a subprocess (stdio transport), discovers its tools,
then starts the LLMAgent with those tools and an interactive REPL.

Usage:
    export OPENROUTER_API_KEY=sk-or-...
    python agent/mcp_client.py
"""

import json
from dotenv import load_dotenv
import os
import subprocess
import sys
import threading
from pathlib import Path

# Make sure sibling imports work when run from repo root
sys.path.insert(0, str(Path(__file__).parent))
from llm_agent import LLMAgent

SERVER_SCRIPT = Path(__file__).parent / "mcp_server.py"
_req_id = 0

load_dotenv()

def _next_id() -> int:
    global _req_id
    _req_id += 1
    return _req_id


class MCPClient:
    """
    Thin stdio MCP client.
    Spawns mcp_server.py and communicates over JSON-RPC 2.0.
    """

    def __init__(self, server_script: Path):
        self.proc = subprocess.Popen(
            [sys.executable, str(server_script)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=sys.stderr,      # forward server logs to our stderr
            text=True,
            bufsize=1,
        )
        self._initialize()

    def _send(self, method: str, params: dict = None) -> dict:
        req = {"jsonrpc": "2.0", "id": _next_id(), "method": method}
        if params is not None:
            req["params"] = params
        self.proc.stdin.write(json.dumps(req) + "\n")
        self.proc.stdin.flush()
        raw = self.proc.stdout.readline()
        return json.loads(raw)

    def _initialize(self):
        resp = self._send("initialize", {
            "protocolVersion": "2024-11-05",
            "clientInfo": {"name": "mcp-client", "version": "0.1.0"},
            "capabilities": {},
        })
        if "error" in resp:
            raise RuntimeError(f"MCP initialize failed: {resp['error']}")

    def list_tools(self) -> list[dict]:
        resp = self._send("tools/list")
        return resp.get("result", {}).get("tools", [])

    def call_tool(self, name: str, arguments: dict) -> str:
        resp = self._send("tools/call", {"name": name, "arguments": arguments})
        if "error" in resp:
            return f"[error] {resp['error']}"
        content = resp.get("result", {}).get("content", [])
        return "\n".join(c.get("text", "") for c in content if c.get("type") == "text")

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.terminate()
        except Exception:
            pass


def _on_tool_call(tool_name: str, args: dict, result: str):
    """Print a compact one-liner whenever the agent uses a tool."""
    arg_summary = ", ".join(f"{k}={v!r}" for k, v in args.items()) if args else ""
    print(f"  ⚙  {tool_name}({arg_summary})")


def main():
    api_key = os.getenv("OPENROUTER_API_KEY")
    if not api_key:
        print("ERROR: OPENROUTER_API_KEY environment variable is not set.")
        sys.exit(1)

    print("Starting MCP server …", end=" ", flush=True)
    client = MCPClient(SERVER_SCRIPT)
    tools = client.list_tools()
    print(f"ready ({len(tools)} tools)\n")

    agent = LLMAgent(
        tools=tools,
        tool_executor=client.call_tool,
        api_key=api_key,
        on_tool_call=_on_tool_call,
    )

    print("xv6 AI Agent  —  chat with your OS. Type 'reset' to clear history, 'exit' to quit.\n")
    print("─" * 60)

    try:
        while True:
            try:
                user_input = input("\nyou > ").strip()
            except (EOFError, KeyboardInterrupt):
                print()
                break

            if not user_input:
                continue
            if user_input.lower() in ("exit", "quit"):
                break
            if user_input.lower() == "reset":
                agent.reset()
                print("  (conversation history cleared)")
                continue

            answer = agent.run(user_input)
            print(f"\nagent > {answer}")

    finally:
        client.close()
        print("\nBye.")


if __name__ == "__main__":
    main()
