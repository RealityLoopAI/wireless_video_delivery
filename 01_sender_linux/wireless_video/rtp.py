from __future__ import annotations

import random
import struct
import time
from collections import OrderedDict

from .models import EncodedPacket, FrameAssembly, RtpPacket


RTP_VERSION = 2
RTP_HEADER_SIZE = 12
PAYLOAD_TYPE_H264 = 96
H264_CLOCK = 90000
EXTENSION_MAGIC = b"OB"
FU_A_TYPE = 28


def _now_ms() -> int:
    return int(time.time() * 1000)


class RtpH264Packetizer:
    def __init__(self, mtu: int, ssrc: int | None = None) -> None:
        self._mtu = mtu
        self._ssrc = ssrc if ssrc is not None else random.randint(1, 0xFFFFFFFF)
        self._seq = random.randint(0, 65535)
        self._base_capture_ts_ms: int | None = None

    def _extension(self, pkt: EncodedPacket, send_ts_ms: int) -> bytes:
        return struct.pack("!2sHQQQ", EXTENSION_MAGIC, 0, pkt.frame_id, pkt.capture_ts_ms, send_ts_ms)

    def _build(self, payload: bytes, timestamp: int, marker: bool, pkt: EncodedPacket, send_ts_ms: int) -> bytes:
        header = bytearray(RTP_HEADER_SIZE)
        header[0] = (RTP_VERSION << 6) | 0x10
        header[1] = PAYLOAD_TYPE_H264 | (0x80 if marker else 0)
        struct.pack_into("!HII", header, 2, self._seq, timestamp, self._ssrc)
        self._seq = (self._seq + 1) & 0xFFFF
        ext_payload = self._extension(pkt, send_ts_ms)
        ext_header = struct.pack("!HH", 0xBEDE, len(ext_payload) // 4)
        return bytes(header) + ext_header + ext_payload + payload

    @staticmethod
    def _split_annexb(payload: bytes) -> list[bytes]:
        if b"\x00\x00\x00\x01" not in payload and b"\x00\x00\x01" not in payload:
            units = []
            offset = 0
            while offset + 4 <= len(payload):
                length = int.from_bytes(payload[offset: offset + 4], "big")
                offset += 4
                if length <= 0 or offset + length > len(payload):
                    break
                units.append(payload[offset: offset + length])
                offset += length
            if units:
                return units
        raw = payload.replace(b"\x00\x00\x00\x01", b"\x00\x00\x01")
        parts = raw.split(b"\x00\x00\x01")
        return [part for part in parts if part]

    def packetize(self, pkt: EncodedPacket) -> list[bytes]:
        if self._base_capture_ts_ms is None:
            self._base_capture_ts_ms = pkt.capture_ts_ms
        delta_ms = max(0, pkt.capture_ts_ms - self._base_capture_ts_ms)
        timestamp = (delta_ms * 90) & 0xFFFFFFFF
        send_ts_ms = _now_ms()
        max_payload = self._mtu - RTP_HEADER_SIZE - 28
        packets: list[bytes] = []
        nal_units = self._split_annexb(pkt.payload)
        for nal_index, nal in enumerate(nal_units):
            is_last_nal = nal_index == len(nal_units) - 1
            if len(nal) <= max_payload:
                packets.append(self._build(nal, timestamp, is_last_nal, pkt, send_ts_ms))
                continue
            nal_header = nal[0]
            fu_indicator = (nal_header & 0xE0) | FU_A_TYPE
            nal_type = nal_header & 0x1F
            offset = 1
            first = True
            while offset < len(nal):
                chunk_size = min(max_payload - 2, len(nal) - offset)
                chunk = nal[offset: offset + chunk_size]
                offset += chunk_size
                start_bit = 0x80 if first else 0x00
                end_bit = 0x40 if offset >= len(nal) else 0x00
                fu_header = bytes([start_bit | end_bit | nal_type])
                marker = is_last_nal and offset >= len(nal)
                packets.append(self._build(bytes([fu_indicator]) + fu_header + chunk, timestamp, marker, pkt, send_ts_ms))
                first = False
        return packets


class RtpH264Depacketizer:
    def __init__(
        self,
        frame_timeout_ms: int = 1000,
        max_frame_buffers: int = 256,
        max_fu_buffers: int = 256,
    ) -> None:
        self._fu_buffers: dict[tuple[int, int], tuple[bytearray, int]] = {}
        self._frame_nals: OrderedDict[int, tuple[bytearray, int]] = OrderedDict()
        self._frame_timeout_ms = max(100, frame_timeout_ms)
        self._max_frame_buffers = max(1, max_frame_buffers)
        self._max_fu_buffers = max(1, max_fu_buffers)
        self._last_gc_ms = 0
        self._last_seq: int | None = None
        self._lost_packets = 0

    @property
    def lost_packets(self) -> int:
        return self._lost_packets

    def parse(self, data: bytes) -> RtpPacket:
        self._housekeep(_now_ms())
        if len(data) < RTP_HEADER_SIZE:
            raise ValueError("RTP packet too short")
        vpxcc = data[0]
        version = vpxcc >> 6
        if version != RTP_VERSION:
            raise ValueError("Unsupported RTP version")
        has_ext = bool(vpxcc & 0x10)
        marker = bool(data[1] & 0x80)
        payload_type = data[1] & 0x7F
        seq, timestamp, ssrc = struct.unpack_from("!HII", data, 2)
        offset = RTP_HEADER_SIZE
        frame_id = 0
        capture_ts_ms = 0
        send_ts_ms = 0
        if has_ext:
            profile, length_words = struct.unpack_from("!HH", data, offset)
            offset += 4
            ext_bytes = length_words * 4
            ext = data[offset: offset + ext_bytes]
            offset += ext_bytes
            if profile == 0xBEDE and ext.startswith(EXTENSION_MAGIC):
                _, _, frame_id, capture_ts_ms, send_ts_ms = struct.unpack("!2sHQQQ", ext)
        payload = data[offset:]
        if self._last_seq is not None and ((self._last_seq + 1) & 0xFFFF) != seq:
            diff = (seq - self._last_seq - 1) & 0xFFFF
            if diff > 0:
                self._lost_packets += diff
        self._last_seq = seq
        return RtpPacket(
            seq=seq,
            timestamp=timestamp,
            marker=marker,
            payload_type=payload_type,
            ssrc=ssrc,
            payload=payload,
            frame_id=frame_id,
            capture_ts_ms=capture_ts_ms,
            send_ts_ms=send_ts_ms,
        )

    def push(self, pkt: RtpPacket) -> FrameAssembly | None:
        if not pkt.payload:
            return None
        now_ms = _now_ms()
        self._housekeep(now_ms)
        nal_type = pkt.payload[0] & 0x1F
        if nal_type == FU_A_TYPE:
            assembly = self._handle_fua(pkt, now_ms)
        else:
            assembly = self._handle_single_nal(pkt, pkt.payload, now_ms)
        self._housekeep(now_ms)
        return assembly

    def _handle_single_nal(self, pkt: RtpPacket, nal: bytes, now_ms: int) -> FrameAssembly | None:
        if pkt.timestamp in self._frame_nals:
            buf = self._frame_nals[pkt.timestamp][0]
        else:
            buf = bytearray()
        buf.extend(b"\x00\x00\x00\x01")
        buf.extend(nal)
        self._frame_nals[pkt.timestamp] = (buf, now_ms)
        if not pkt.marker:
            return None
        payload = bytes(self._frame_nals.pop(pkt.timestamp)[0])
        return FrameAssembly(
            frame_id=pkt.frame_id,
            capture_ts_ms=pkt.capture_ts_ms,
            payload=payload,
            is_keyframe=self._contains_idr(payload),
            arrival_ts_ms=now_ms,
            timestamp=pkt.timestamp,
            seq_end=pkt.seq,
        )

    def _handle_fua(self, pkt: RtpPacket, now_ms: int) -> FrameAssembly | None:
        if len(pkt.payload) < 2:
            return None
        fu_indicator = pkt.payload[0]
        fu_header = pkt.payload[1]
        start = bool(fu_header & 0x80)
        end = bool(fu_header & 0x40)
        nal_type = fu_header & 0x1F
        key = (pkt.timestamp, nal_type)
        if start:
            nal_header = bytes([(fu_indicator & 0xE0) | nal_type])
            self._fu_buffers[key] = (bytearray(nal_header + pkt.payload[2:]), now_ms)
        elif key in self._fu_buffers:
            buf, _ = self._fu_buffers[key]
            buf.extend(pkt.payload[2:])
            self._fu_buffers[key] = (buf, now_ms)
        else:
            return None
        if not end:
            return None
        nal = bytes(self._fu_buffers.pop(key)[0])
        return self._handle_single_nal(pkt, nal, now_ms)

    def _housekeep(self, now_ms: int) -> None:
        run_stale_cleanup = now_ms - self._last_gc_ms >= 50
        if run_stale_cleanup:
            self._last_gc_ms = now_ms
            cutoff = now_ms - self._frame_timeout_ms

            if self._frame_nals:
                stale_timestamps = [ts for ts, (_, updated_ms) in self._frame_nals.items() if updated_ms < cutoff]
                for timestamp in stale_timestamps:
                    self._frame_nals.pop(timestamp, None)

            if self._fu_buffers:
                stale_keys = [key for key, (_, updated_ms) in self._fu_buffers.items() if updated_ms < cutoff]
                for key in stale_keys:
                    self._fu_buffers.pop(key, None)

        while len(self._frame_nals) > self._max_frame_buffers:
            self._frame_nals.popitem(last=False)

        overflow = len(self._fu_buffers) - self._max_fu_buffers
        if overflow > 0:
            # Drop oldest partial FU buffers first when jitter/loss spikes.
            oldest = sorted(self._fu_buffers.items(), key=lambda item: item[1][1])[:overflow]
            for key, _ in oldest:
                self._fu_buffers.pop(key, None)

    @staticmethod
    def _contains_idr(payload: bytes) -> bool:
        parts = payload.split(b"\x00\x00\x00\x01")
        for part in parts:
            if part and (part[0] & 0x1F) == 5:
                return True
        return False
