from __future__ import annotations

import errno
import socket

from .models import RtpPacket
from .rtp import RtpH264Depacketizer


class TransportTx:
    def __init__(self) -> None:
        self._sock: socket.socket | None = None
        self._remote: tuple[str, int] | None = None

    def open(self, remote_ip: str, port: int, ttl: int, buffer_bytes: int) -> bool:
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, buffer_bytes)
        self._sock.setsockopt(socket.IPPROTO_IP, socket.IP_TTL, ttl)
        # Real-time first: never block encode thread on congested uplink.
        self._sock.setblocking(False)
        self._remote = (remote_ip, port)
        return True

    def send(self, payload: bytes) -> bool:
        if self._sock is None or self._remote is None:
            raise RuntimeError("transport not open")
        try:
            self._sock.sendto(payload, self._remote)
            return True
        except (BlockingIOError, InterruptedError):
            return False
        except OSError as exc:
            if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK, errno.ENOBUFS):
                return False
            raise

    def close(self) -> None:
        if self._sock is not None:
            self._sock.close()
        self._sock = None
        self._remote = None


class TransportRx:
    def __init__(self) -> None:
        self._sock: socket.socket | None = None
        self._parser = RtpH264Depacketizer()

    @property
    def lost_packets(self) -> int:
        return self._parser.lost_packets

    def open(
        self,
        listen_ip: str,
        port: int,
        buffer_bytes: int,
        timeout_ms: int,
        depacketizer_timeout_ms: int = 1000,
        depacketizer_max_frames: int = 256,
        depacketizer_max_fus: int = 256,
    ) -> bool:
        self._parser = RtpH264Depacketizer(
            frame_timeout_ms=depacketizer_timeout_ms,
            max_frame_buffers=depacketizer_max_frames,
            max_fu_buffers=depacketizer_max_fus,
        )
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, buffer_bytes)
        self._sock.bind((listen_ip, port))
        self._sock.settimeout(timeout_ms / 1000.0)
        return True

    def recv(self) -> RtpPacket | None:
        if self._sock is None:
            return None
        try:
            data, _ = self._sock.recvfrom(65535)
        except socket.timeout:
            return None
        return self._parser.parse(data)

    def push(self, pkt: RtpPacket):
        return self._parser.push(pkt)

    def close(self) -> None:
        if self._sock is not None:
            self._sock.close()
        self._sock = None
