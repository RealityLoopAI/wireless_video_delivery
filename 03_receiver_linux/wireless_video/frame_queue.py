from __future__ import annotations

import queue
import threading
from typing import Generic, Optional, TypeVar


T = TypeVar("T")


class FrameQueue(Generic[T]):
    def __init__(self, max_size: int) -> None:
        self._queue: queue.Queue[T] = queue.Queue(maxsize=max_size)
        self._lock = threading.Lock()
        self._drops = 0

    @property
    def drops(self) -> int:
        with self._lock:
            return self._drops

    def push_latest(self, item: T) -> None:
        with self._lock:
            if self._queue.full():
                try:
                    self._queue.get_nowait()
                    self._drops += 1
                except queue.Empty:
                    pass
            self._queue.put_nowait(item)

    def pop(self, timeout_ms: int) -> Optional[T]:
        try:
            return self._queue.get(timeout=timeout_ms / 1000.0)
        except queue.Empty:
            return None

    def size(self) -> int:
        return self._queue.qsize()

