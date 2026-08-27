#!/usr/bin/env python3
from __future__ import annotations

import collections
from dataclasses import dataclass
import threading
import time
from typing import Callable


@dataclass(frozen=True)
class RebuildDecision:
    accepted: bool
    reason: str
    request_id: str
    local_packet_age_seconds: float | None


class CaptureRebuildGuard:
    def __init__(
        self,
        stale_seconds: float,
        *,
        clock: Callable[[], float] = time.monotonic,
        recent_request_limit: int = 64,
    ):
        self._stale_seconds = max(0.1, float(stale_seconds))
        self._clock = clock
        self._recent_request_limit = max(8, int(recent_request_limit))
        self._lock = threading.Lock()
        self._last_local_packet_at: float | None = None
        self._pending_request_id: str | None = None
        self._rebuild_in_progress = False
        self._recent_request_ids: collections.deque[str] = collections.deque()
        self._recent_request_set: set[str] = set()

    def mark_local_packet(self, observed_at: float | None = None) -> None:
        with self._lock:
            self._last_local_packet_at = self._clock() if observed_at is None else observed_at

    def local_packet_age_seconds(self, now: float | None = None) -> float | None:
        with self._lock:
            return self._local_packet_age_locked(self._clock() if now is None else now)

    def request_rebuild(self, request_id: str, now: float | None = None) -> RebuildDecision:
        observed_at = self._clock() if now is None else now
        normalized_id = request_id.strip() or "legacy"
        with self._lock:
            age = self._local_packet_age_locked(observed_at)
            if normalized_id in self._recent_request_set:
                return RebuildDecision(False, "duplicate_request", normalized_id, age)
            self._remember_request_locked(normalized_id)
            if self._pending_request_id is not None or self._rebuild_in_progress:
                return RebuildDecision(False, "rebuild_already_pending", normalized_id, age)
            if age is not None and age < self._stale_seconds:
                return RebuildDecision(False, "local_capture_healthy", normalized_id, age)
            self._pending_request_id = normalized_id
            return RebuildDecision(True, "local_capture_stale", normalized_id, age)

    def begin_rebuild(self) -> str | None:
        with self._lock:
            if self._pending_request_id is None or self._rebuild_in_progress:
                return None
            request_id = self._pending_request_id
            self._pending_request_id = None
            self._rebuild_in_progress = True
            return request_id

    def complete_rebuild(self) -> None:
        with self._lock:
            self._rebuild_in_progress = False

    def _local_packet_age_locked(self, now: float) -> float | None:
        if self._last_local_packet_at is None:
            return None
        return max(0.0, now - self._last_local_packet_at)

    def _remember_request_locked(self, request_id: str) -> None:
        self._recent_request_ids.append(request_id)
        self._recent_request_set.add(request_id)
        while len(self._recent_request_ids) > self._recent_request_limit:
            expired = self._recent_request_ids.popleft()
            self._recent_request_set.discard(expired)
