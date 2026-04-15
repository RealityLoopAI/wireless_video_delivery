from __future__ import annotations

from dataclasses import dataclass


@dataclass
class RuntimeState:
    name: str = "INIT"
    consecutive_failures: int = 0
    reconnect_index: int = 0

    def mark_success(self) -> None:
        self.name = "RUNNING"
        self.consecutive_failures = 0
        self.reconnect_index = 0

    def mark_failure(self, degraded_threshold: int) -> None:
        self.consecutive_failures += 1
        if self.consecutive_failures >= degraded_threshold:
            self.name = "DEGRADED"

    def enter_reconnecting(self) -> None:
        self.name = "RECONNECTING"

    def next_backoff_ms(self, backoffs: list[int]) -> int:
        idx = min(self.reconnect_index, len(backoffs) - 1)
        value = backoffs[idx]
        self.reconnect_index += 1
        return value

