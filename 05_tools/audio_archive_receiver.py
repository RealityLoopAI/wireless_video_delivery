#!/usr/bin/env python3
"""Receive duplicated Opus RTP streams and publish wall-clock audio segments."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import logging
import os
import shutil
import signal
import socket
import struct
import threading
import time
import urllib.parse
import urllib.request
import uuid
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any


LOG = logging.getLogger("gwv3.audio_archive")
OPUS_SILENCE_20_MS = b"\xf8\xff\xfe"
TIMING_CSV_HEADER = [
    "global_timestamp_us",
    "window_start_global_us",
    "window_end_global_us",
    "expected_packets",
    "received_packets",
    "silence_packets",
    "first_source_sequence",
    "last_source_sequence",
    "first_source_rtp_timestamp",
    "last_source_rtp_timestamp",
    "first_sender_system_timestamp_us",
    "last_sender_system_timestamp_us",
    "first_receiver_receive_timestamp_us",
    "last_receiver_receive_timestamp_us",
    "clock_sync_valid",
    "sender_offset_us",
    "sender_delay_us",
    "sender_drift_ppm",
    "late_packets",
    "duplicate_packets",
]


def now_us() -> int:
    return time.time_ns() // 1000


def fsync_file(path: Path) -> None:
    with path.open("rb") as handle:
        os.fsync(handle.fileno())


def fsync_dir(path: Path) -> None:
    try:
        descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
    except OSError:
        pass


def atomic_json(path: Path, value: dict[str, Any]) -> None:
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8") as handle:
        json.dump(value, handle, ensure_ascii=False, indent=2, sort_keys=True)
        handle.write("\n")
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, path)
    fsync_dir(path.parent)


def write_json(path: Path, value: dict[str, Any]) -> None:
    with path.open("w", encoding="utf-8") as handle:
        json.dump(value, handle, ensure_ascii=False, indent=2, sort_keys=True)
        handle.write("\n")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            block = handle.read(1024 * 1024)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def signed_rtp_delta(value: int, reference: int) -> int:
    delta = (value - reference) & 0xFFFFFFFF
    return delta - 0x100000000 if delta & 0x80000000 else delta


@dataclass
class RtpPacket:
    sequence: int
    timestamp: int
    ssrc: int
    payload_type: int
    marker: bool
    payload: bytes
    receiver_receive_us: int
    sender_system_us: int = 0
    global_us: int = 0
    clock_valid: bool = False
    offset_us: int = 0
    delay_us: int = 0
    drift_ppm: float = 0.0


def parse_rtp(data: bytes, receive_us: int) -> RtpPacket | None:
    if len(data) < 12:
        return None
    first, second, sequence, timestamp, ssrc = struct.unpack("!BBHII", data[:12])
    if first >> 6 != 2:
        return None
    csrc_count = first & 0x0F
    offset = 12 + csrc_count * 4
    if len(data) < offset:
        return None
    if first & 0x10:
        if len(data) < offset + 4:
            return None
        extension_words = struct.unpack("!H", data[offset + 2 : offset + 4])[0]
        offset += 4 + extension_words * 4
        if len(data) < offset:
            return None
    end = len(data)
    if first & 0x20:
        padding = data[-1]
        if padding == 0 or padding > end - offset:
            return None
        end -= padding
    return RtpPacket(
        sequence=sequence,
        timestamp=timestamp,
        ssrc=ssrc,
        payload_type=second & 0x7F,
        marker=bool(second & 0x80),
        payload=data[offset:end],
        receiver_receive_us=receive_us,
    )


def make_rtp(sequence: int, timestamp: int, ssrc: int, payload_type: int, payload: bytes) -> bytes:
    return struct.pack(
        "!BBHII",
        0x80,
        payload_type & 0x7F,
        sequence & 0xFFFF,
        timestamp & 0xFFFFFFFF,
        ssrc & 0xFFFFFFFF,
    ) + payload


def _build_ogg_crc_table() -> list[int]:
    table = []
    for value in range(256):
        remainder = value << 24
        for _ in range(8):
            remainder = ((remainder << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if remainder & 0x80000000 else (remainder << 1) & 0xFFFFFFFF
        table.append(remainder)
    return table


OGG_CRC_TABLE = _build_ogg_crc_table()


def ogg_crc(data: bytes) -> int:
    checksum = 0
    for value in data:
        checksum = ((checksum << 8) & 0xFFFFFFFF) ^ OGG_CRC_TABLE[((checksum >> 24) ^ value) & 0xFF]
    return checksum


class OggOpusWriter:
    """Minimal Ogg muxer for already packetized 20 ms Opus frames."""

    def __init__(self, path: Path, sample_rate: int, channels: int = 1, pre_skip: int = 312):
        self.path = path
        self.sample_rate = sample_rate
        self.channels = channels
        self.pre_skip = pre_skip
        self.serial = int.from_bytes(os.urandom(4), "little")
        self.page_sequence = 0
        self.total_samples = 0
        self.handle = path.open("wb")
        self._packets: list[bytes] = []
        self._pending_page: tuple[list[bytes], int] | None = None
        opus_head = (
            b"OpusHead"
            + struct.pack("<BBHIhB", 1, channels, pre_skip, sample_rate, 0, 0)
        )
        vendor = b"GWV3 native Ogg/Opus muxer"
        opus_tags = b"OpusTags" + struct.pack("<I", len(vendor)) + vendor + struct.pack("<I", 0)
        self._write_page([opus_head], granule_position=0, header_type=0x02)
        self._write_page([opus_tags], granule_position=0, header_type=0)

    @staticmethod
    def _lacing(packet: bytes) -> list[int]:
        size = len(packet)
        values = [255] * (size // 255)
        values.append(size % 255)
        return values

    def _write_page(self, packets: list[bytes], granule_position: int, header_type: int) -> None:
        lacing = [value for packet in packets for value in self._lacing(packet)]
        if len(lacing) > 255:
            raise ValueError("too many Ogg lacing values in one page")
        body = b"".join(packets)
        header = (
            b"OggS"
            + bytes((0, header_type))
            + struct.pack("<QIIIB", granule_position, self.serial, self.page_sequence, 0, len(lacing))
            + bytes(lacing)
        )
        page = bytearray(header + body)
        struct.pack_into("<I", page, 22, ogg_crc(page))
        self.handle.write(page)
        self.page_sequence += 1

    def _queue_page(self) -> None:
        if not self._packets:
            return
        if self._pending_page is not None:
            packets, granule = self._pending_page
            self._write_page(packets, granule, 0)
        self._pending_page = (self._packets, self.pre_skip + self.total_samples)
        self._packets = []

    def write_packet(self, payload: bytes, frame_samples: int) -> None:
        if not payload:
            raise ValueError("empty Opus payload")
        self._packets.append(payload)
        self.total_samples += frame_samples
        lacing_count = sum(len(self._lacing(packet)) for packet in self._packets)
        if len(self._packets) >= 50 or lacing_count >= 240:
            self._queue_page()

    def close(self) -> None:
        self._queue_page()
        if self._pending_page is not None:
            packets, granule = self._pending_page
            self._write_page(packets, granule, 0x04)
            self._pending_page = None
        self.handle.flush()
        self.handle.close()


@dataclass
class ClockModel:
    valid: bool = False
    offset_us: int = 0
    delay_us: int = 0
    drift_ppm: float = 0.0
    last_sync_us: int = 0
    receiver_update_us: int = 0


@dataclass
class TimingAnchor:
    stream_instance_id: str
    rtp_timestamp: int
    sender_system_us: int
    sender_send_us: int
    receiver_receive_us: int
    sample_rate: int
    ssrc: int


class TimingRegistry:
    def __init__(self, receiver_admin_url: str, model_timeout_ms: int):
        self.receiver_admin_url = receiver_admin_url.rstrip("/")
        self.model_timeout_us = model_timeout_ms * 1000
        self._lock = threading.Lock()
        self._models: dict[str, ClockModel] = {}
        self._anchors: dict[str, TimingAnchor] = {}
        self.last_admin_error = ""

    def update_anchor(self, sender_id: str, data: dict[str, Any], receive_us: int) -> None:
        anchor = TimingAnchor(
            stream_instance_id=str(data.get("stream_instance_id", "")),
            rtp_timestamp=int(data["rtp_timestamp"]) & 0xFFFFFFFF,
            sender_system_us=int(data["sender_system_timestamp_us"]),
            sender_send_us=int(data.get("sender_send_timestamp_us", 0)),
            receiver_receive_us=receive_us,
            sample_rate=int(data.get("sample_rate", 48000)),
            ssrc=int(data.get("ssrc", 0)) & 0xFFFFFFFF,
        )
        with self._lock:
            self._anchors[sender_id] = anchor

    def refresh_models(self) -> None:
        try:
            with urllib.request.urlopen(
                self.receiver_admin_url + "/api/status",
                timeout=1.0,
            ) as response:
                status = json.loads(response.read().decode("utf-8"))
            models: dict[str, ClockModel] = {}
            update_us = now_us()
            for item in status.get("clock_sync", []):
                sender_id = str(item.get("sender_id", ""))
                if not sender_id:
                    continue
                models[sender_id] = ClockModel(
                    valid=bool(item.get("clock_sync_valid", False)),
                    offset_us=int(item.get("clock_offset_us", 0)),
                    delay_us=int(item.get("clock_delay_us", 0)),
                    drift_ppm=float(item.get("clock_drift_ppm", 0.0)),
                    last_sync_us=int(item.get("clock_last_sync_us", 0)),
                    receiver_update_us=update_us,
                )
            with self._lock:
                self._models = models
                self.last_admin_error = ""
        except Exception as exc:
            with self._lock:
                self.last_admin_error = str(exc)

    def map_packet(self, sender_id: str, packet: RtpPacket) -> RtpPacket:
        with self._lock:
            anchor = self._anchors.get(sender_id)
            model = self._models.get(sender_id, ClockModel())
        model_fresh = (
            model.receiver_update_us > 0
            and now_us() - model.receiver_update_us <= self.model_timeout_us
        )
        valid = model.valid and model_fresh
        if anchor and anchor.sample_rate > 0 and anchor.ssrc == packet.ssrc:
            sample_delta = signed_rtp_delta(packet.timestamp, anchor.rtp_timestamp)
            sender_us = anchor.sender_system_us + round(
                sample_delta * 1_000_000 / anchor.sample_rate
            )
            packet.sender_system_us = sender_us
            packet.global_us = sender_us + (model.offset_us if valid else 0)
        else:
            packet.sender_system_us = 0
            packet.global_us = packet.receiver_receive_us
        packet.clock_valid = valid and anchor is not None
        packet.offset_us = model.offset_us
        packet.delay_us = model.delay_us
        packet.drift_ppm = model.drift_ppm
        return packet

    def snapshot(self, sender_id: str) -> dict[str, Any]:
        with self._lock:
            anchor = self._anchors.get(sender_id)
            model = self._models.get(sender_id, ClockModel())
            error = self.last_admin_error
        model_fresh = (
            model.receiver_update_us > 0
            and now_us() - model.receiver_update_us <= self.model_timeout_us
        )
        return {
            "clock_sync_valid": model.valid and model_fresh and anchor is not None,
            "clock_offset_us": model.offset_us,
            "clock_delay_us": model.delay_us,
            "clock_drift_ppm": model.drift_ppm,
            "clock_last_sync_us": model.last_sync_us,
            "timing_anchor_available": anchor is not None,
            "timing_stream_instance_id": anchor.stream_instance_id if anchor else "",
            "timing_anchor_receiver_us": anchor.receiver_receive_us if anchor else 0,
            "receiver_admin_error": error,
        }


@dataclass
class SecondStats:
    second_us: int
    expected: int = 0
    received: int = 0
    silence: int = 0
    first_sequence: int | None = None
    last_sequence: int | None = None
    first_rtp_timestamp: int | None = None
    last_rtp_timestamp: int | None = None
    first_sender_us: int = 0
    last_sender_us: int = 0
    first_receiver_us: int = 0
    last_receiver_us: int = 0
    clock_valid: bool = False
    offset_us: int = 0
    delay_us: int = 0
    drift_ppm: float = 0.0
    late_packets: int = 0
    duplicate_packets: int = 0

    def add(self, packet: RtpPacket | None) -> None:
        self.expected += 1
        if packet is None:
            self.silence += 1
            return
        self.received += 1
        if self.first_sequence is None:
            self.first_sequence = packet.sequence
            self.first_rtp_timestamp = packet.timestamp
            self.first_sender_us = packet.sender_system_us
            self.first_receiver_us = packet.receiver_receive_us
        self.last_sequence = packet.sequence
        self.last_rtp_timestamp = packet.timestamp
        self.last_sender_us = packet.sender_system_us
        self.last_receiver_us = packet.receiver_receive_us
        self.clock_valid = packet.clock_valid
        self.offset_us = packet.offset_us
        self.delay_us = packet.delay_us
        self.drift_ppm = packet.drift_ppm

    def row(self) -> list[Any]:
        return [
            self.second_us,
            self.second_us,
            self.second_us + 1_000_000,
            self.expected,
            self.received,
            self.silence,
            "" if self.first_sequence is None else self.first_sequence,
            "" if self.last_sequence is None else self.last_sequence,
            "" if self.first_rtp_timestamp is None else self.first_rtp_timestamp,
            "" if self.last_rtp_timestamp is None else self.last_rtp_timestamp,
            self.first_sender_us,
            self.last_sender_us,
            self.first_receiver_us,
            self.last_receiver_us,
            1 if self.clock_valid else 0,
            self.offset_us,
            self.delay_us,
            f"{self.drift_ppm:.6f}",
            self.late_packets,
            self.duplicate_packets,
        ]


class OpusSegment:
    def __init__(
        self,
        staging_root: Path,
        sender_id: str,
        window_start_us: int,
        window_end_us: int,
        first_slot_us: int,
        sample_rate: int,
        payload_type: int,
        output_ssrc: int,
    ):
        start_time = time.localtime(window_start_us / 1_000_000)
        self.sender_id = sender_id
        self.window_start_us = window_start_us
        self.window_end_us = window_end_us
        self.first_slot_us = first_slot_us
        self.last_slot_us = 0
        self.sample_rate = sample_rate
        self.payload_type = payload_type
        self.output_ssrc = output_ssrc
        self.segment_id = uuid.uuid4().hex
        self.date_text = time.strftime("%Y-%m-%d", start_time)
        self.time_text = time.strftime("%H%M%S", start_time)
        parent = staging_root / "segments" / sender_id / self.date_text
        parent.mkdir(parents=True, exist_ok=True)
        self.inprogress_dir = parent / f"{self.time_text}.{self.segment_id}.inprogress"
        self.staged_dir = parent / f"{self.time_text}.{self.segment_id}.staged"
        self.finalize_dir = parent / f"{self.time_text}.{self.segment_id}.finalize"
        self.inprogress_dir.mkdir()
        self.audio_path = self.inprogress_dir / "audio.opus.inprogress"
        self.csv_path = self.inprogress_dir / "audio_timing.csv"
        self.csv_handle = self.csv_path.open("w", encoding="utf-8", newline="")
        self.csv_writer = csv.writer(self.csv_handle)
        self.csv_writer.writerow(TIMING_CSV_HEADER)
        self.total_expected = 0
        self.total_received = 0
        self.total_silence = 0
        self.total_late = 0
        self.total_duplicate = 0
        self.clock_valid_packets = 0
        self.current_second: SecondStats | None = None
        self.writer = OggOpusWriter(self.audio_path, sample_rate)

    def write_slot(self, slot_us: int, output_sequence: int, output_timestamp: int, packet: RtpPacket | None) -> None:
        second_us = (slot_us // 1_000_000) * 1_000_000
        if self.current_second is None or self.current_second.second_us != second_us:
            self.flush_second()
            self.current_second = SecondStats(second_us=second_us)
        payload = packet.payload if packet is not None else OPUS_SILENCE_20_MS
        del output_sequence, output_timestamp
        frame_samples = round(self.sample_rate * 20 / 1000)
        self.writer.write_packet(payload, frame_samples)
        self.current_second.add(packet)
        self.total_expected += 1
        if packet is None:
            self.total_silence += 1
        else:
            self.total_received += 1
            if packet.clock_valid:
                self.clock_valid_packets += 1
        self.last_slot_us = slot_us

    def add_discard_counts(self, late: int, duplicate: int) -> None:
        self.total_late += late
        self.total_duplicate += duplicate
        if self.current_second is not None:
            self.current_second.late_packets += late
            self.current_second.duplicate_packets += duplicate

    def flush_second(self) -> None:
        if self.current_second is None:
            return
        self.csv_writer.writerow(self.current_second.row())
        self.csv_handle.flush()
        self.current_second = None

    def close(self, reason: str) -> Path | None:
        self.flush_second()
        self.csv_handle.flush()
        self.csv_handle.close()
        self.writer.close()
        if self.audio_path.exists():
            final_audio = self.inprogress_dir / "audio.opus"
            os.replace(self.audio_path, final_audio)
        else:
            LOG.error("audio segment missing sender=%s", self.sender_id)
            shutil.rmtree(self.inprogress_dir, ignore_errors=True)
            return None
        duration_us = max(0, self.last_slot_us + 20_000 - self.first_slot_us)
        meta = {
            "schema_version": 1,
            "sender_id": self.sender_id,
            "segment_id": self.segment_id,
            "codec": "opus",
            "container": "ogg",
            "muxer": "gwv3_native_ogg_opus",
            "global_timestamp_source": "sender_system_timestamp_us_plus_clock_offset",
            "sender_timestamp_origin": "rtp_anchor_estimated_frame_start",
            "sample_rate": self.sample_rate,
            "channels": 1,
            "frame_duration_ms": 20,
            "payload_type": self.payload_type,
            "ssrc": self.output_ssrc,
            "segment_window_start_global_us": self.window_start_us,
            "segment_window_end_global_us": self.window_end_us,
            "first_audio_global_us": self.first_slot_us,
            "last_audio_global_us": self.last_slot_us,
            "audio_duration_us": duration_us,
            "partial_start": self.first_slot_us > self.window_start_us + 20_000,
            "partial_end": self.last_slot_us + 20_000 < self.window_end_us,
            "close_reason": reason,
            "expected_packets": self.total_expected,
            "received_packets": self.total_received,
            "silence_packets": self.total_silence,
            "late_packets": self.total_late,
            "duplicate_packets": self.total_duplicate,
            "clock_sync_valid_packets": self.clock_valid_packets,
            "created_receiver_us": now_us(),
        }
        write_json(self.inprogress_dir / "audio_meta.json", meta)
        os.replace(self.inprogress_dir, self.finalize_dir)
        return self.finalize_dir


@dataclass
class StreamConfig:
    sender_id: str
    port: int
    ssrc: int
    payload_type: int = 111
    sample_rate: int = 48000


class AudioStreamRecorder:
    def __init__(self, config: StreamConfig, app: "AudioArchiveService"):
        self.config = config
        self.app = app
        self.frame_us = app.config["frame_duration_ms"] * 1000
        self.frame_samples = round(config.sample_rate * self.frame_us / 1_000_000)
        self._stop = threading.Event()
        self._enabled = True
        self._enabled_lock = threading.Lock()
        self._buffer_lock = threading.Lock()
        self._buffer: dict[int, RtpPacket] = {}
        self._duplicate_pending = 0
        self._late_pending = 0
        self._socket: socket.socket | None = None
        self._listener = threading.Thread(target=self._listen, name=f"audio-rtp-{config.sender_id}", daemon=True)
        self._scheduler = threading.Thread(target=self._schedule, name=f"audio-mux-{config.sender_id}", daemon=True)
        self._segment: OpusSegment | None = None
        self._next_slot: int | None = None
        self._output_sequence = 0
        self._output_timestamp = 0
        self.received_packets = 0
        self.invalid_packets = 0
        self.ssrc_mismatches = 0
        self.payload_type_mismatches = 0
        self.last_packet_receiver_us = 0
        self.last_packet_global_us = 0
        self.last_error = ""
        self.completed_segments = 0

    def start(self) -> None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2 * 1024 * 1024)
        sock.bind((self.app.config["bind_ip"], self.config.port))
        sock.settimeout(0.2)
        self._socket = sock
        self._listener.start()
        self._scheduler.start()

    def stop(self) -> None:
        self._stop.set()
        if self._socket is not None:
            self._socket.close()
        self._listener.join(timeout=2.0)
        self._scheduler.join(timeout=12.0)

    def set_enabled(self, enabled: bool) -> None:
        with self._enabled_lock:
            self._enabled = enabled

    def enabled(self) -> bool:
        with self._enabled_lock:
            return self._enabled

    def _listen(self) -> None:
        sock = self._socket
        if sock is None:
            self.last_error = "RTP socket was not initialized"
            return
        LOG.info("audio RTP listening sender=%s port=%d", self.config.sender_id, self.config.port)
        while not self._stop.is_set():
            try:
                data, _address = sock.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                break
            receive_us = now_us()
            packet = parse_rtp(data, receive_us)
            if packet is None or not packet.payload:
                self.invalid_packets += 1
                continue
            if packet.ssrc != self.config.ssrc:
                self.ssrc_mismatches += 1
                continue
            if packet.payload_type != self.config.payload_type:
                self.payload_type_mismatches += 1
                continue
            packet = self.app.timing.map_packet(self.config.sender_id, packet)
            slot = round(packet.global_us / self.frame_us)
            with self._buffer_lock:
                if self._next_slot is not None and slot < self._next_slot:
                    self._late_pending += 1
                    continue
                if slot in self._buffer:
                    self._duplicate_pending += 1
                    if packet.receiver_receive_us >= self._buffer[slot].receiver_receive_us:
                        continue
                self._buffer[slot] = packet
                newest_allowed = slot - self.app.config["max_buffer_seconds"] * 1_000_000 // self.frame_us
                for old_slot in [value for value in self._buffer if value < newest_allowed]:
                    del self._buffer[old_slot]
                    self._late_pending += 1
            self.received_packets += 1
            self.last_packet_receiver_us = receive_us
            self.last_packet_global_us = packet.global_us

    def _schedule(self) -> None:
        jitter_us = self.app.config["jitter_buffer_ms"] * 1000
        while not self._stop.is_set():
            if not self.enabled() or self.app.storage_blocked():
                self._close_segment("archive_paused" if not self.enabled() else "storage_blocked")
                self._next_slot = None
                time.sleep(0.1)
                continue
            ready_slot = (now_us() - jitter_us) // self.frame_us
            if self._next_slot is None:
                self._next_slot = ready_slot
            if self._next_slot > ready_slot:
                time.sleep(min(0.01, (self._next_slot - ready_slot) * self.frame_us / 1_000_000))
                continue
            slot = self._next_slot
            slot_us = slot * self.frame_us
            segment_seconds = self.app.config["segment_seconds"]
            window_start_us = (slot_us // (segment_seconds * 1_000_000)) * segment_seconds * 1_000_000
            if self._segment is None or self._segment.window_start_us != window_start_us:
                self._close_segment("wall_clock_boundary")
                try:
                    self._segment = OpusSegment(
                        self.app.staging_root,
                        self.config.sender_id,
                        window_start_us,
                        window_start_us + segment_seconds * 1_000_000,
                        slot_us,
                        self.config.sample_rate,
                        self.config.payload_type,
                        self.config.ssrc,
                    )
                except Exception as exc:
                    self.last_error = str(exc)
                    LOG.exception("audio segment start failed sender=%s", self.config.sender_id)
                    self.app.set_runtime_storage_error(str(exc))
                    time.sleep(1.0)
                    self._next_slot = ready_slot + 1
                    continue
            with self._buffer_lock:
                packet = self._buffer.pop(slot, None)
                late = self._late_pending
                duplicate = self._duplicate_pending
                self._late_pending = 0
                self._duplicate_pending = 0
                stale = [value for value in self._buffer if value < slot]
                for old_slot in stale:
                    del self._buffer[old_slot]
                    late += 1
            try:
                self._segment.add_discard_counts(late, duplicate)
                self._segment.write_slot(
                    slot_us,
                    self._output_sequence,
                    self._output_timestamp,
                    packet,
                )
                self._output_sequence = (self._output_sequence + 1) & 0xFFFF
                self._output_timestamp = (self._output_timestamp + self.frame_samples) & 0xFFFFFFFF
                self._next_slot += 1
            except Exception as exc:
                self.last_error = str(exc)
                LOG.exception("audio segment write failed sender=%s", self.config.sender_id)
                self._close_segment("write_error")
                self._next_slot = ready_slot + 1
        self._close_segment("service_stop")

    def _close_segment(self, reason: str) -> None:
        segment = self._segment
        if segment is None:
            return
        self._segment = None
        try:
            result = segment.close(reason)
            if result is not None:
                self.completed_segments += 1
                self.app.uploader_wakeup.set()
                LOG.info("audio segment queued for finalize sender=%s path=%s", self.config.sender_id, result)
        except Exception as exc:
            self.last_error = str(exc)
            LOG.exception("audio segment close failed sender=%s", self.config.sender_id)

    def status(self) -> dict[str, Any]:
        with self._buffer_lock:
            buffered = len(self._buffer)
        return {
            "sender_id": self.config.sender_id,
            "port": self.config.port,
            "ssrc": self.config.ssrc,
            "enabled": self.enabled(),
            "recording": self._segment is not None,
            "segment_window_start_global_us": self._segment.window_start_us if self._segment else 0,
            "received_packets": self.received_packets,
            "invalid_packets": self.invalid_packets,
            "ssrc_mismatches": self.ssrc_mismatches,
            "payload_type_mismatches": self.payload_type_mismatches,
            "buffered_packets": buffered,
            "last_packet_receiver_us": self.last_packet_receiver_us,
            "last_packet_global_us": self.last_packet_global_us,
            "completed_segments": self.completed_segments,
            "last_error": self.last_error,
            **self.app.timing.snapshot(self.config.sender_id),
        }


class AudioUploader:
    def __init__(self, app: "AudioArchiveService"):
        self.app = app
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="audio-nas-uploader", daemon=True)
        self.published_segments = 0
        self.failed_uploads = 0
        self.last_success_us = 0
        self.last_error = ""
        self.nas_available = False
        self.nas_free_bytes = 0

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self.app.uploader_wakeup.set()
        self._thread.join(timeout=15.0)

    def _run(self) -> None:
        while not self._stop.is_set():
            self.app.refresh_storage_state()
            try:
                self._nas_ready()
                if not list(self.app.staging_root.glob("segments/*/*/*.staged")):
                    self.last_error = ""
            except Exception as exc:
                self.last_error = str(exc)
            for pending in sorted(self.app.staging_root.glob("segments/*/*/*.finalize")):
                if self._stop.is_set():
                    break
                try:
                    self._stage(pending)
                except Exception as exc:
                    self.failed_uploads += 1
                    self.last_error = str(exc)
                    LOG.warning("audio local finalize deferred path=%s error=%s", pending, exc)
                    break
            for staged in sorted(self.app.staging_root.glob("segments/*/*/*.staged")):
                if self._stop.is_set():
                    break
                try:
                    self._publish(staged)
                    self.published_segments += 1
                    self.last_success_us = now_us()
                    self.last_error = ""
                except Exception as exc:
                    self.failed_uploads += 1
                    self.last_error = str(exc)
                    LOG.warning("audio NAS publish deferred path=%s error=%s", staged, exc)
                    break
            self.app.uploader_wakeup.wait(self.app.config["upload_interval_seconds"])
            self.app.uploader_wakeup.clear()

    def _stage(self, pending: Path) -> Path:
        meta = json.loads((pending / "audio_meta.json").read_text(encoding="utf-8"))
        segment_id = str(meta.get("segment_id") or pending.name.split(".")[-2])
        files = {}
        for name in ("audio.opus", "audio_timing.csv", "audio_meta.json"):
            path = pending / name
            if not path.exists() or path.stat().st_size == 0:
                raise RuntimeError(f"audio segment file missing or empty: {path}")
            fsync_file(path)
            files[name] = {"size": path.stat().st_size, "sha256": sha256_file(path)}
        atomic_json(
            pending / "audio_staged.json",
            {
                "schema_version": 1,
                "segment_id": segment_id,
                "staged_receiver_us": now_us(),
                "files": files,
            },
        )
        fsync_dir(pending)
        staged = pending.with_name(pending.name.rsplit(".", 1)[0] + ".staged")
        os.replace(pending, staged)
        fsync_dir(staged.parent)
        return staged

    def _nas_ready(self) -> None:
        nas_root = self.app.nas_root
        if self.app.config["nas_require_mount"] and not os.path.ismount(nas_root):
            self.nas_available = False
            raise RuntimeError(f"NAS mount unavailable: {nas_root}")
        if not nas_root.exists():
            self.nas_available = False
            raise RuntimeError(f"NAS root unavailable: {nas_root}")
        usage = shutil.disk_usage(nas_root)
        self.nas_free_bytes = usage.free
        self.nas_available = True
        warning_bytes = self.app.config["nas_low_space_warning_mb"] * 1024 * 1024
        if warning_bytes and usage.free < warning_bytes:
            LOG.warning("NAS free space low free_bytes=%d", usage.free)

    def _publish(self, staged: Path) -> None:
        self._nas_ready()
        sender_id = staged.parents[1].name
        date_text = staged.parent.name
        time_text = staged.name.split(".", 1)[0]
        final_parent = self.app.nas_root / self.app.config["nas_subdirectory"] / sender_id / date_text
        final = final_parent / time_text
        marker = json.loads((staged / "audio_staged.json").read_text(encoding="utf-8"))
        incoming_meta = json.loads((staged / "audio_meta.json").read_text(encoding="utf-8"))
        replace_existing = False
        displaced_target: Path | None = None
        if (final / "audio_ready.json").exists():
            ready = json.loads((final / "audio_ready.json").read_text(encoding="utf-8"))
            if ready.get("files") == marker.get("files"):
                shutil.rmtree(staged)
                return
            existing_meta = json.loads((final / "audio_meta.json").read_text(encoding="utf-8"))
            existing_duration = int(existing_meta.get("audio_duration_us", 0))
            incoming_duration = int(incoming_meta.get("audio_duration_us", 0))
            if incoming_duration > existing_duration:
                replace_existing = True
                displaced_target = self._unique_partial_target(
                    final_parent,
                    time_text,
                    str(ready.get("segment_id", "existing")),
                )
            else:
                final = self._unique_partial_target(
                    final_parent,
                    time_text,
                    str(marker["segment_id"]),
                )
        hidden = (
            self.app.nas_root
            / ".gwv3_audio_uploading"
            / sender_id
            / date_text
            / f"{time_text}.{marker['segment_id']}"
        )
        shutil.rmtree(hidden, ignore_errors=True)
        hidden.mkdir(parents=True, exist_ok=True)
        for name, expected in marker["files"].items():
            source = staged / name
            target = hidden / name
            shutil.copyfile(source, target)
            fsync_file(target)
            if target.stat().st_size != expected["size"] or sha256_file(target) != expected["sha256"]:
                raise RuntimeError(f"NAS verification failed: {target}")
        ready = {
            "schema_version": 1,
            "sender_id": sender_id,
            "segment_id": marker["segment_id"],
            "published_receiver_us": now_us(),
            "files": marker["files"],
        }
        atomic_json(hidden / "audio_ready.json", ready)
        final.parent.mkdir(parents=True, exist_ok=True)
        fsync_dir(hidden)
        if replace_existing:
            assert displaced_target is not None
            os.replace(final, displaced_target)
        os.replace(hidden, final)
        fsync_dir(final.parent)
        shutil.rmtree(staged)

    @staticmethod
    def _unique_partial_target(parent: Path, time_text: str, segment_id: str) -> Path:
        prefix = f"{time_text}-partial-{segment_id[:8]}"
        candidate = parent / prefix
        suffix = 1
        while candidate.exists():
            candidate = parent / f"{prefix}-{suffix}"
            suffix += 1
        return candidate

    def status(self) -> dict[str, Any]:
        return {
            "nas_available": self.nas_available,
            "nas_free_bytes": self.nas_free_bytes,
            "published_segments": self.published_segments,
            "failed_uploads": self.failed_uploads,
            "last_success_us": self.last_success_us,
            "last_error": self.last_error,
        }


class AudioArchiveService:
    def __init__(self, config: dict[str, Any]):
        self.config = config
        self.staging_root = Path(config["staging_root"]).expanduser()
        self.nas_root = Path(config["nas_root"]).expanduser()
        self.staging_root.mkdir(parents=True, exist_ok=True)
        self.timing = TimingRegistry(config["receiver_admin_url"], config["clock_model_timeout_ms"])
        self.uploader_wakeup = threading.Event()
        self._stop = threading.Event()
        self._state_lock = threading.Lock()
        self._storage_blocked = False
        self._storage_reason = ""
        self._runtime_storage_error = ""
        self.streams = {
            item["sender_id"]: AudioStreamRecorder(StreamConfig(**item), self)
            for item in config["streams"]
        }
        self.uploader = AudioUploader(self)
        self._timing_thread = threading.Thread(target=self._timing_loop, name="audio-timing", daemon=True)
        self._clock_thread = threading.Thread(target=self._clock_loop, name="audio-clock-model", daemon=True)
        self._admin_server: ThreadingHTTPServer | None = None
        self._admin_thread: threading.Thread | None = None
        self._timing_socket: socket.socket | None = None

    def start(self) -> None:
        self._recover_inprogress_segments()
        self.refresh_storage_state()
        timing_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        timing_socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)
        timing_socket.bind((self.config["bind_ip"], self.config["timing_port"]))
        timing_socket.settimeout(0.2)
        self._timing_socket = timing_socket
        self._start_admin()
        self._timing_thread.start()
        self._clock_thread.start()
        self.uploader.start()
        for stream in self.streams.values():
            stream.start()

    def _recover_inprogress_segments(self) -> None:
        for directory in self.staging_root.glob("segments/*/*/*.inprogress"):
            try:
                audio = directory / "audio.opus"
                unfinished_audio = directory / "audio.opus.inprogress"
                if not audio.exists() and unfinished_audio.exists() and unfinished_audio.stat().st_size > 0:
                    os.replace(unfinished_audio, audio)
                (directory / "input.sdp").unlink(missing_ok=True)
                timing = directory / "audio_timing.csv"
                if not audio.exists() or not timing.exists():
                    failed = directory.with_name(directory.name.rsplit(".", 1)[0] + ".failed")
                    os.replace(directory, failed)
                    LOG.warning("unrecoverable audio segment retained path=%s", failed)
                    continue
                parts = directory.name.split(".")
                segment_id = parts[-2] if len(parts) >= 3 else uuid.uuid4().hex
                sender_id = directory.parents[1].name
                meta_path = directory / "audio_meta.json"
                if not meta_path.exists():
                    write_json(
                        meta_path,
                        {
                            "schema_version": 1,
                            "sender_id": sender_id,
                            "segment_id": segment_id,
                            "codec": "opus",
                            "container": "ogg",
                            "recovered_after_unclean_stop": True,
                            "partial_end": True,
                            "close_reason": "service_recovery",
                            "created_receiver_us": now_us(),
                        },
                    )
                finalize = directory.with_name(directory.name.rsplit(".", 1)[0] + ".finalize")
                os.replace(directory, finalize)
                LOG.info("recovered audio segment path=%s", finalize)
            except Exception:
                LOG.exception("audio segment recovery failed path=%s", directory)

    def stop(self) -> None:
        self._stop.set()
        if self._timing_socket is not None:
            self._timing_socket.close()
        if self._admin_server is not None:
            self._admin_server.shutdown()
            self._admin_server.server_close()
        for stream in self.streams.values():
            stream.stop()
        self.uploader.stop()
        self._timing_thread.join(timeout=2.0)
        self._clock_thread.join(timeout=2.0)
        if self._admin_thread is not None:
            self._admin_thread.join(timeout=2.0)

    def set_runtime_storage_error(self, error: str) -> None:
        with self._state_lock:
            self._runtime_storage_error = error

    def storage_blocked(self) -> bool:
        with self._state_lock:
            return self._storage_blocked

    def refresh_storage_state(self) -> None:
        usage = shutil.disk_usage(self.staging_root)
        minimum = self.config["min_free_disk_mb"] * 1024 * 1024
        oldest_age_days = 0.0
        pending = list(self.staging_root.glob("segments/*/*/*.staged"))
        pending += list(self.staging_root.glob("segments/*/*/*.finalize"))
        if pending:
            oldest = min(path.stat().st_mtime for path in pending)
            oldest_age_days = (time.time() - oldest) / 86400.0
        blocked = usage.free < minimum or oldest_age_days >= self.config["local_retention_days"]
        reason = ""
        if usage.free < minimum:
            reason = f"local free space below {self.config['min_free_disk_mb']} MB"
        elif oldest_age_days >= self.config["local_retention_days"]:
            reason = f"oldest pending audio is {oldest_age_days:.2f} days old"
        with self._state_lock:
            self._storage_blocked = blocked
            self._storage_reason = reason

    def _timing_loop(self) -> None:
        sock = self._timing_socket
        if sock is None:
            return
        LOG.info("audio timing listening port=%d", self.config["timing_port"])
        try:
            while not self._stop.is_set():
                try:
                    data, _address = sock.recvfrom(8192)
                except socket.timeout:
                    continue
                except OSError:
                    break
                receive_us = now_us()
                try:
                    report = json.loads(data.decode("utf-8"))
                    sender_id = str(report.get("sender_id", ""))
                    if report.get("message_type") != "audio_timing_anchor" or sender_id not in self.streams:
                        continue
                    if int(report.get("ssrc", -1)) != self.streams[sender_id].config.ssrc:
                        continue
                    self.timing.update_anchor(sender_id, report, receive_us)
                except (ValueError, KeyError, TypeError, json.JSONDecodeError):
                    continue
        finally:
            sock.close()

    def _clock_loop(self) -> None:
        while not self._stop.is_set():
            self.timing.refresh_models()
            self._stop.wait(1.0)

    def _start_admin(self) -> None:
        app = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self) -> None:
                path = urllib.parse.urlsplit(self.path).path
                if path in ("/api/status", "/api/audio/status"):
                    self.reply(200, app.status())
                else:
                    self.reply(404, {"ok": False, "error": "not found"})

            def do_POST(self) -> None:
                parsed = urllib.parse.urlsplit(self.path)
                args = urllib.parse.parse_qs(parsed.query)
                sender_id = args.get("sender_id", [""])[0]
                if parsed.path in ("/api/start-all", "/api/audio/start-all"):
                    app.set_all_enabled(True)
                    self.reply(200, app.status())
                elif parsed.path in ("/api/stop-all", "/api/audio/stop-all"):
                    app.set_all_enabled(False)
                    self.reply(200, app.status())
                elif parsed.path in ("/api/start-sender", "/api/audio/start-sender"):
                    self.sender_action(sender_id, True)
                elif parsed.path in ("/api/stop-sender", "/api/audio/stop-sender"):
                    self.sender_action(sender_id, False)
                else:
                    self.reply(404, {"ok": False, "error": "not found"})

            def sender_action(self, sender_id: str, enabled: bool) -> None:
                stream = app.streams.get(sender_id)
                if stream is None:
                    self.reply(404, {"ok": False, "error": "unknown sender_id"})
                    return
                stream.set_enabled(enabled)
                self.reply(200, {"ok": True, "sender_id": sender_id, "enabled": enabled})

            def reply(self, status: int, value: dict[str, Any]) -> None:
                body = json.dumps(value, ensure_ascii=False).encode("utf-8")
                self.send_response(status)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, _format: str, *_args: Any) -> None:
                return

        self._admin_server = ThreadingHTTPServer(
            (self.config["admin_bind_ip"], self.config["admin_port"]),
            Handler,
        )
        self._admin_thread = threading.Thread(
            target=self._admin_server.serve_forever,
            name="audio-admin",
            daemon=True,
        )
        self._admin_thread.start()

    def set_all_enabled(self, enabled: bool) -> None:
        for stream in self.streams.values():
            stream.set_enabled(enabled)

    def status(self) -> dict[str, Any]:
        self.refresh_storage_state()
        staged = list(self.staging_root.glob("segments/*/*/*.staged"))
        finalizing = list(self.staging_root.glob("segments/*/*/*.finalize"))
        pending = staged + finalizing
        staged_bytes = sum(
            path.stat().st_size
            for directory in pending
            for path in directory.rglob("*")
            if path.is_file()
        )
        usage = shutil.disk_usage(self.staging_root)
        with self._state_lock:
            blocked = self._storage_blocked
            reason = self._storage_reason
            runtime_error = self._runtime_storage_error
        streams = [stream.status() for stream in self.streams.values()]
        return {
            "ok": True,
            "service": "gwv3-audio-archive",
            "archive_enabled": any(item["enabled"] for item in streams),
            "storage_blocked": blocked,
            "storage_block_reason": reason,
            "runtime_storage_error": runtime_error,
            "staging_root": str(self.staging_root),
            "staging_free_bytes": usage.free,
            "pending_segments": len(pending),
            "local_finalize_pending_segments": len(finalizing),
            "pending_bytes": staged_bytes,
            "segment_seconds": self.config["segment_seconds"],
            "timing_port": self.config["timing_port"],
            "streams": streams,
            **self.uploader.status(),
        }


def load_config(path: Path) -> dict[str, Any]:
    raw = json.loads(path.read_text(encoding="utf-8"))
    defaults = {
        "bind_ip": "0.0.0.0",
        "timing_port": 50130,
        "admin_bind_ip": "127.0.0.1",
        "admin_port": 18083,
        "receiver_admin_url": "http://127.0.0.1:18080",
        "clock_model_timeout_ms": 10000,
        "staging_root": "/home/fz/audio_staging",
        "nas_root": "/home/fz/Desktop/nas",
        "nas_subdirectory": "audio",
        "nas_require_mount": True,
        "nas_low_space_warning_mb": 51200,
        "segment_seconds": 900,
        "jitter_buffer_ms": 250,
        "frame_duration_ms": 20,
        "max_buffer_seconds": 30,
        "local_retention_days": 7,
        "min_free_disk_mb": 5120,
        "upload_interval_seconds": 2.0,
        "streams": [],
    }
    config = {**defaults, **raw}
    if not config["streams"]:
        raise ValueError("audio archive config requires at least one stream")
    ports = [int(item["port"]) for item in config["streams"]]
    sender_ids = [str(item["sender_id"]) for item in config["streams"]]
    if len(ports) != len(set(ports)) or len(sender_ids) != len(set(sender_ids)):
        raise ValueError("audio archive stream ports and sender IDs must be unique")
    if config["frame_duration_ms"] != 20:
        raise ValueError("only 20 ms Opus frames are supported")
    return config


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--log-level", default="INFO")
    args = parser.parse_args()
    logging.basicConfig(
        level=getattr(logging, args.log_level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(name)s %(message)s",
    )
    service = AudioArchiveService(load_config(args.config))
    stop = threading.Event()

    def handle_signal(_signum: int, _frame: Any) -> None:
        stop.set()

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)
    service.start()
    LOG.info("audio archive service started streams=%d", len(service.streams))
    try:
        while not stop.wait(1.0):
            pass
    finally:
        service.stop()
    LOG.info("audio archive service stopped")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
