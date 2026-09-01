#!/usr/bin/env python3
from pathlib import Path


def main():
    root = Path(__file__).resolve().parents[1]
    sender = (root / "01_sender_linux" / "src" / "main.cpp").read_text(encoding="utf-8")
    receiver = (root / "02_receiver_linux" / "src" / "main.cpp").read_text(encoding="utf-8")
    frontend = (root / "09_web_monitor" / "static" / "index.html").read_text(encoding="utf-8")

    assert "config.web_rgb_preview.fps >= camera.config.rgb_profile.fps" in sender
    assert "camera.next_web_rgb_preview = now" in sender
    assert "kRgbH264ClientMaxLagPackets = 12" in receiver
    assert "send_all_with_timeout" in receiver
    assert "kRgbH264ClientSendBufferBytes = 32 * 1024" in receiver
    assert "configure_rgb_h264_client_socket(fd)" in receiver
    assert "TCP_NOTSENT_LOWAT" in receiver
    assert "waiting_for_keyframe = true" in receiver
    assert "next_seq + kRgbH264ClientMaxLagPackets < newest_next_seq" in receiver
    assert "next_seq = newest_next_seq" in receiver
    assert "main_request_seq = cam->rgb_stream.next_seq" in receiver
    assert "preview_request_seq = cam->rgb_preview_stream.next_seq" in receiver
    assert "H264_MAX_DECODE_QUEUE = 2" in frontend
    assert "this.decoder.decodeQueueSize > H264_MAX_DECODE_QUEUE" in frontend
    assert "this.decoder = null" in frontend
    assert "this.configured = false" in frontend
    assert "this.awaitingKeyframe && !isKey" in frontend
    assert "H264_MAX_INPUT_BUFFER_BYTES" in frontend
    assert "metadata=global" in frontend
    assert "const quality = 'preview'" in frontend
    assert "H264_MAX_TIMELINE_LAG_US = 400 * 1000" in frontend
    assert "H264_TIMELINE_REBASE_INTERVAL_MS = 30 * 1000" in frontend
    assert "H264_STALE_RECONNECT_MS = 250" in frontend
    assert "this.markFailed('RGB 视频重连')" in frontend
    assert "if (!this.stopped)" in frontend
    assert "this.frameIsStale(timestampUs)" in frontend
    assert "this.droppingStaleFrames" in frontend
    assert "/api/audio/status" in frontend
    assert "/api/audio/start-all" in frontend
    assert "/api/audio/stop-sender" in frontend
    print("web preview low-latency guard test passed")


if __name__ == "__main__":
    main()
