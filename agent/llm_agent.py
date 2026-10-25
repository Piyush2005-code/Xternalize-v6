"""
llm_agent.py

LLM Agent class backed by OpenRouter API.
Handles the agentic loop: call the model, detect tool calls, execute them,
feed results back, and repeat until the model produces a final text answer.

Conversation history is kept on the instance so the agent remembers previous
turns — making it a proper multi-turn chat bot.
"""

import json
import os
import requests
from typing import Any

OPENROUTER_API_URL = "https://openrouter.ai/api/v1/chat/completions"
DEFAULT_MODEL = "openai/gpt-4o-mini"


class LLMAgent:
    """
    A stateful agentic loop over OpenRouter-compatible chat completions.

    Each call to run() appends to self.history so the model sees the full
    conversation, making it behave like a persistent chat bot.

    Usage:
        agent = LLMAgent(tools=tool_definitions, tool_executor=call_tool_fn)
        print(agent.run("list the files in /"))
        print(agent.run("now read the README"))   # agent remembers context
        agent.reset()                              # start fresh if needed
    """

    def __init__(
        self,
        tools: list[dict],
        tool_executor,                      # callable(tool_name, args) -> str
        api_key: str | None = None,
        model: str = DEFAULT_MODEL,
        system_prompt: str = (
            "You are a helpful assistant that controls an xv6 OS running inside QEMU. "
            "Use the provided tools to execute commands and inspect the OS, then explain "
            "the results clearly to the user."
        ),
        max_iterations: int = 10,
        on_tool_call=None,                  # optional callback(tool_name, args, result)
    ):
        self.tools = tools
        self.tool_executor = tool_executor
        self.api_key = api_key or os.environ.get("OPENROUTER_API_KEY", "")
        self.model = model
        self.system_prompt = system_prompt
        self.max_iterations = max_iterations
        self.on_tool_call = on_tool_call

        if not self.api_key:
            raise ValueError("OPENROUTER_API_KEY is not set.")

        # Persistent conversation history (system prompt + all turns)
        self.history: list[dict] = [{"role": "system", "content": self.system_prompt}]

    # ------------------------------------------------------------------
    # Public
    # ------------------------------------------------------------------

    def run(self, user_message: str) -> str:
        """
        Append user_message to history, run the agentic loop, append the
        assistant reply, and return the final text answer.
        """
        self.history.append({"role": "user", "content": user_message})

        for _ in range(self.max_iterations):
            response = self._chat(self.history)
            choice = response["choices"][0]
            message = choice["message"]

            # Persist the assistant turn in history
            self.history.append(message)

            # Model is done — return the final answer
            if choice["finish_reason"] != "tool_calls" or not message.get("tool_calls"):
                return message.get("content") or ""

            # Execute each tool call and feed results back into history
            for tc in message["tool_calls"]:
                tool_name = tc["function"]["name"]
                try:
                    args: dict[str, Any] = json.loads(tc["function"]["arguments"])
                except json.JSONDecodeError:
                    args = {}

                result = self.tool_executor(tool_name, args)

                if self.on_tool_call:
                    self.on_tool_call(tool_name, args, result)

                self.history.append({
                    "role": "tool",
                    "tool_call_id": tc["id"],
                    "content": str(result),
                })

        return "[agent] Max iterations reached without a final answer."

    def reset(self):
        """Clear conversation history (keeps the system prompt)."""
        self.history = [{"role": "system", "content": self.system_prompt}]

    # ------------------------------------------------------------------
    # Private
    # ------------------------------------------------------------------

    def _chat(self, messages: list[dict]) -> dict:
        """Send a chat completion request to OpenRouter."""
        headers = {
            "Authorization": f"Bearer {self.api_key}",
            "Content-Type": "application/json",
        }
        payload: dict[str, Any] = {
            "model": self.model,
            "messages": messages,
        }
        if self.tools:
            payload["tools"] = [{"type": "function", "function": t} for t in self.tools]
            payload["tool_choice"] = "auto"

        resp = requests.post(OPENROUTER_API_URL, headers=headers, json=payload, timeout=60)
        resp.raise_for_status()
        return resp.json()
