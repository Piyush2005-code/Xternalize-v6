"""
llm_agent.py

LLM Agent class backed by OpenRouter API.
Handles the agentic loop: call the model, detect tool calls, execute them,
feed results back, and repeat until the model produces a final text answer.
"""

import json
import os
import requests
from typing import Any

OPENROUTER_API_URL = "https://openrouter.ai/api/v1/chat/completions"
DEFAULT_MODEL = "openai/gpt-4o-mini"


class LLMAgent:
    """
    A simple agentic loop over OpenRouter-compatible chat completions.

    Usage:
        agent = LLMAgent(tools=tool_definitions, tool_executor=call_tool_fn)
        response = agent.run("ls /")
    """

    def __init__(
        self,
        tools: list[dict],
        tool_executor,                      # callable(tool_name, args) -> str
        api_key: str | None = None,
        model: str = DEFAULT_MODEL,
        system_prompt: str = "You are a helpful assistant that controls an xv6 OS via tools.",
        max_iterations: int = 10,
    ):
        self.tools = tools
        self.tool_executor = tool_executor
        self.api_key = api_key or os.environ.get("OPENROUTER_API_KEY", "")
        self.model = model
        self.system_prompt = system_prompt
        self.max_iterations = max_iterations

        if not self.api_key:
            raise ValueError("OPENROUTER_API_KEY is not set.")

    # ------------------------------------------------------------------
    # Public
    # ------------------------------------------------------------------

    def run(self, user_message: str) -> str:
        """Run the agentic loop for a single user turn and return final text."""
        messages = [
            {"role": "system", "content": self.system_prompt},
            {"role": "user", "content": user_message},
        ]

        for _ in range(self.max_iterations):
            response = self._chat(messages)
            choice = response["choices"][0]
            message = choice["message"]

            # Append the assistant turn (may or may not have tool_calls)
            messages.append(message)

            # If model is done, return the text
            if choice["finish_reason"] != "tool_calls" or not message.get("tool_calls"):
                return message.get("content") or ""

            # Execute each tool call and feed results back
            for tc in message["tool_calls"]:
                tool_name = tc["function"]["name"]
                try:
                    args: dict[str, Any] = json.loads(tc["function"]["arguments"])
                except json.JSONDecodeError:
                    args = {}

                print(f"[agent] calling tool '{tool_name}' args={args}")
                result = self.tool_executor(tool_name, args)
                print(f"[agent] tool result: {result!r}")

                messages.append({
                    "role": "tool",
                    "tool_call_id": tc["id"],
                    "content": str(result),
                })

        return "[agent] Max iterations reached without a final answer."

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
