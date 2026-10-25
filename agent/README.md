# xv6 AI Agent

An LLM agent (via OpenRouter) that controls a live xv6 OS running in QEMU,
using MCP tools over a TCP bridge to the QEMU serial console.

## Architecture

```
User prompt
   │
   ▼
LLMAgent          (llm_agent.py)   – agentic loop over OpenRouter chat API
   │  tool_call
   ▼
MCPClient         (mcp_client.py)  – spawns mcp_server.py, talks JSON-RPC stdio
   │
   ▼
MCPServer         (mcp_server.py)  – exposes xv6 tools, connects to bridge :4444
   │  TCP :4444
   ▼
qemu_bridge.py                     – relays commands to xv6 serial console
   │  TCP :5555  (raw serial)
   ▼
QEMU  ──serial──▶  xv6 shell
```

## Setup

```bash
pip install -r agent/requirements.txt
export OPENROUTER_API_KEY=sk-or-...
```

## Running

Open **three terminals** in the repo root:

**Terminal 1 — build & start xv6 with serial exposed**
```bash
make qemu-bridge          # QEMU listens on :5555
```

**Terminal 2 — start the bridge** (waits for xv6 boot prompt, then listens on :4444)
```bash
python agent/qemu_bridge.py
```

**Terminal 3 — start the agent REPL**
```bash
python agent/mcp_client.py
```

Then type natural-language commands:
```
you> list the files in the root directory
you> run echo hello from xv6
you> read the file README
```

## Environment variables

| Variable            | Default     | Description                        |
|---------------------|-------------|------------------------------------|
| `OPENROUTER_API_KEY`| —           | Required. Your OpenRouter API key. |
| `XV6_SERIAL_PORT`   | `5555`      | Port QEMU exposes serial on.       |
| `QEMU_BRIDGE_HOST`  | `127.0.0.1` | Bridge listen address.             |
| `QEMU_BRIDGE_PORT`  | `4444`      | Bridge listen port.                |

## Files

| File               | Role                                      |
|--------------------|-------------------------------------------|
| `llm_agent.py`     | LLM agent class (OpenRouter, tool loop)   |
| `mcp_server.py`    | MCP server — xv6 tool definitions + I/O  |
| `mcp_client.py`    | MCP client + interactive REPL             |
| `qemu_bridge.py`   | TCP bridge: MCP ↔ QEMU serial console    |
| `requirements.txt` | Python dependencies (`requests`)          |
