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
    def __init__(self) -> None:
        self._fu_buffers: dict[tuple[int, int], bytearray] = {}
        self._frame_nals: OrderedDict[int, bytearray] = OrderedDict()
        self._last_seq: int | None = None
        self._lost_packets = 0

    @property
    def lost_packets(self) -> int:
        return self._lost_packets

    def parse(self, data: bytes) -> RtpPacket:
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
        nal_type = pkt.payload[0] & 0x1F
        if nal_type == FU_A_TYPE:
            return self._handle_fua(pkt)
        return self._handle_single_nal(pkt, pkt.payload)

    def _handle_single_nal(self, pkt: RtpPacket, nal: bytes) -> FrameAssembly | None:
        buf = self._frame_nals.setdefault(pkt.timestamp, bytearray())
        buf.extend(b"\x00\x00\x00\x01")
        buf.extend(nal)
        if not pkt.marker:
            return None
        payload = bytes(buf)
        del self._frame_nals[pkt.timestamp]
        return FrameAssembly(
            frame_id=pkt.frame_id,
            capture_ts_ms=pkt.capture_ts_ms,
            payload=payload,
            is_keyframe=self._contains_idr(payload),
            arrival_ts_ms=_now_ms(),
            timestamp=pkt.timestamp,
            seq_end=pkt.seq,
        )

    def _handle_fua(self, pkt: RtpPacket) -> FrameAssembly | None:
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
            self._fu_buffers[key] = bytearray(nal_header + pkt.payload[2:])
        elif key in self._fu_buffers:
            self._fu_buffers[key].extend(pkt.payload[2:])
        else:
            return None
        if not end:
            return None
        nal = bytes(self._fu_buffers.pop(key))
        return self._handle_single_nal(pkt, nal)

    @staticmethod
    def _contains_idr(payload: bytes) -> bool:
        parts = payload.split(b"\x00\x00\x00\x01")
        for part in parts:
            if part and (part[0] & 0x1F) == 5:
                return True
        return False
