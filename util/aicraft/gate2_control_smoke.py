#!/usr/bin/env python3
# Added for AICraft on 2026-07-27; see AICRAFT_CHANGES.md.
"""Drive the real AICraft Agent client over its Unix control bridge."""

from __future__ import annotations

import argparse
import hmac
import json
import os
import signal
import socket
import time
from pathlib import Path
from typing import Any


class SmokeFailure(RuntimeError):
    pass


class ControlHarness:
    def __init__(self, socket_path: Path, session_id: str, secret: str) -> None:
        self.socket_path = socket_path
        self.session_id = session_id
        self.secret = secret
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(str(socket_path))
        os.chmod(socket_path, 0o600)
        self.listener.listen(1)
        self.listener.settimeout(40)
        self.peer: socket.socket | None = None
        self.buffer = bytearray()
        self.last_observation: dict[str, Any] | None = None
        self.last_observation_sequence = 0
        self.observation_count = 0
        self.action_counter = 0
        self.completed_actions: list[str] = []

    def close(self) -> None:
        if self.peer is not None:
            self.peer.close()
        self.listener.close()
        try:
            self.socket_path.unlink()
        except FileNotFoundError:
            pass

    def accept_and_register(self) -> None:
        self.peer, _ = self.listener.accept()
        hello = self.receive(40)
        if hello.get("type") != "agent.hello":
            raise SmokeFailure(f"expected agent.hello, received {hello!r}")
        expected_capabilities = {"action.lifecycle.v1", "observation.client.v1"}
        if (
            hello.get("protocol") != "aicraft-control-v1"
            or hello.get("session_id") != self.session_id
            or hello.get("state") != "CONTROL_READY"
            or hello.get("ready") is not True
            or not expected_capabilities.issubset(set(hello.get("capabilities", [])))
            or not hmac.compare_digest(str(hello.get("session_secret", "")), self.secret)
        ):
            raise SmokeFailure(f"invalid authenticated hello: {hello!r}")
        self.send(
            {
                "type": "agent.registered",
                "session_id": self.session_id,
                "observation_stream": True,
            }
        )

    def send(self, message: dict[str, Any]) -> None:
        if self.peer is None:
            raise SmokeFailure("control peer is not connected")
        encoded = json.dumps(message, separators=(",", ":")).encode() + b"\n"
        self.peer.sendall(encoded)

    def receive(self, timeout: float) -> dict[str, Any]:
        if self.peer is None:
            raise SmokeFailure("control peer is not connected")
        deadline = time.monotonic() + timeout
        while True:
            newline = self.buffer.find(b"\n")
            if newline >= 0:
                encoded = bytes(self.buffer[:newline])
                del self.buffer[: newline + 1]
                try:
                    message = json.loads(encoded)
                except json.JSONDecodeError as error:
                    raise SmokeFailure(f"invalid JSON from engine: {error}") from error
                if not isinstance(message, dict):
                    raise SmokeFailure(f"non-object message from engine: {message!r}")
                if message.get("type") == "agent.observation":
                    self.record_observation(message)
                return message
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise SmokeFailure("timed out waiting for engine control message")
            self.peer.settimeout(remaining)
            chunk = self.peer.recv(65536)
            if not chunk:
                raise SmokeFailure("engine disconnected its control socket")
            self.buffer.extend(chunk)
            if len(self.buffer) > 8 * 1024 * 1024:
                raise SmokeFailure("engine control message exceeded 8 MiB")

    def record_observation(self, message: dict[str, Any]) -> None:
        sequence = message.get("sequence")
        observation = message.get("observation")
        if (
            message.get("protocol") != "aicraft-control-v1"
            or message.get("session_id") != self.session_id
            or not isinstance(sequence, int)
            or sequence <= self.last_observation_sequence
            or not isinstance(observation, dict)
        ):
            raise SmokeFailure(f"invalid or non-monotonic observation: {message!r}")
        self.last_observation_sequence = sequence
        self.last_observation = observation
        self.observation_count += 1

    def wait_for_fixture(self) -> None:
        deadline = time.monotonic() + 45
        required = {
            "mcl_core:cobble",
            "mcl_tools:pick_iron",
            "aicraft_gate2_smoke:use_token",
        }
        while time.monotonic() < deadline:
            message = self.receive(deadline - time.monotonic())
            if message.get("type") != "agent.observation":
                continue
            inventory = message["observation"].get("inventory", [])
            items = {
                entry.get("item")
                for entry in inventory
                if isinstance(entry, dict)
            }
            position = message["observation"].get("self", {}).get("position", {})
            if (
                required.issubset(items)
                and abs(float(position.get("x", 99))) < 1
                and abs(float(position.get("y", 99)) - 10) < 1
                and abs(float(position.get("z", 99))) < 1
            ):
                return
        raise SmokeFailure("Mineclonia fixture never appeared in client observation")

    def run_action(
        self, action_type: str, fields: dict[str, Any], timeout: float = 15
    ) -> None:
        self.action_counter += 1
        action_id = f"gate2-{self.action_counter:02d}-{action_type}"
        message = {
            "type": action_type,
            "action_id": action_id,
            "intent": f"Gate 2 Mineclonia smoke: {action_type}",
            **fields,
        }
        self.send(message)
        expected = ["accepted", "executing"]
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            incoming = self.receive(deadline - time.monotonic())
            if incoming.get("type") != "action.status":
                continue
            if incoming.get("session_id") != self.session_id:
                raise SmokeFailure(f"spoofed status session: {incoming!r}")
            if incoming.get("action_id") != action_id:
                raise SmokeFailure(f"status for unexpected action: {incoming!r}")
            status = incoming.get("status")
            if expected:
                required = expected.pop(0)
                if status != required:
                    raise SmokeFailure(
                        f"{action_id} expected {required}, received {incoming!r}"
                    )
                continue
            if status != "succeeded":
                raise SmokeFailure(f"{action_id} did not succeed: {incoming!r}")
            self.completed_actions.append(action_type)
            return
        raise SmokeFailure(f"{action_id} did not reach a terminal state")

    def wait_for_inventory(self, item: str, slot: int, count: int) -> None:
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            message = self.receive(deadline - time.monotonic())
            if message.get("type") != "agent.observation":
                continue
            for entry in message["observation"].get("inventory", []):
                if (
                    isinstance(entry, dict)
                    and entry.get("item") == item
                    and entry.get("slot") == slot
                    and entry.get("count") == count
                ):
                    return
        raise SmokeFailure(
            f"inventory never showed {item} x{count} in zero-based slot {slot}"
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("socket_path", type=Path)
    parser.add_argument("session_id")
    parser.add_argument("secret_file", type=Path)
    parser.add_argument("done_file", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    secret = args.secret_file.read_text(encoding="utf-8").rstrip("\r\n")
    harness = ControlHarness(args.socket_path, args.session_id, secret)
    try:
        harness.accept_and_register()
        harness.wait_for_fixture()
        harness.run_action("chat", {"message": "AICraft Gate 2 Mineclonia smoke"})
        harness.run_action(
            "inventory_move",
            {
                "from": {
                    "inventory": "current_player",
                    "list": "main",
                    "slot": 0,
                },
                "to": {
                    "inventory": "current_player",
                    "list": "main",
                    "slot": 1,
                },
                "count": 2,
            },
        )
        harness.wait_for_inventory("mcl_core:cobble", 1, 2)
        harness.run_action("equip", {"slot": 2})
        harness.run_action(
            "use",
            {
                "item": "aicraft_gate2_smoke:use_token",
                "target": {
                    "kind": "node",
                    "position": {"x": 0, "y": 9, "z": 0},
                    "face": {"x": 0, "y": 1, "z": 0},
                },
            },
        )
        harness.wait_for_inventory("aicraft_gate2_smoke:use_token", 2, 1)
        harness.run_action(
            "place",
            {
                "item": "mcl_core:cobble",
                "target": {
                    "kind": "node",
                    "position": {"x": 1, "y": 9, "z": 0},
                    "face": {"x": 0, "y": 1, "z": 0},
                },
            },
        )
        harness.run_action(
            "dig",
            {
                "item": "mcl_tools:pick_iron",
                "target": {
                    "kind": "node",
                    "position": {"x": 1, "y": 10, "z": 0},
                },
            },
            timeout=35,
        )
        harness.run_action("look", {"yaw": 0, "pitch": 35})
        harness.run_action(
            "move",
            {
                "direction": {"forward": 0.5, "right": 0},
                "jump": False,
                "sneak": False,
                "duration_ms": 600,
            },
        )
        harness.run_action("wait", {"duration_ms": 100})
        if harness.observation_count < 3:
            raise SmokeFailure("client observation stream did not remain active")
        print(
            json.dumps(
                {
                    "ok": True,
                    "actions": harness.completed_actions,
                    "client_observations": harness.observation_count,
                    "last_observation_sequence": harness.last_observation_sequence,
                },
                separators=(",", ":"),
            ),
            flush=True,
        )
        args.done_file.write_text("ok\n", encoding="utf-8")
        # Keep the authenticated bridge open until the shell has checked both
        # client and authoritative server evidence. Its cleanup sends SIGTERM
        # to the Agent before terminating this harness.
        stopping = False

        def request_stop(_signum: int, _frame: Any) -> None:
            nonlocal stopping
            stopping = True

        signal.signal(signal.SIGTERM, request_stop)
        while not stopping:
            time.sleep(1)
        return 0
    finally:
        harness.close()


if __name__ == "__main__":
    raise SystemExit(main())
