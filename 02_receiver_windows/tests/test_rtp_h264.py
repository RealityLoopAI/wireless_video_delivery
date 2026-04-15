from wireless_video.models import EncodedPacket
from wireless_video.rtp import RtpH264Depacketizer, RtpH264Packetizer


def test_packetize_and_depacketize_single_nal() -> None:
    packetizer = RtpH264Packetizer(mtu=1200, ssrc=1234)
    depacketizer = RtpH264Depacketizer()
    payload = b"\x00\x00\x00\x01\x65" + b"\xAA" * 100
    encoded = EncodedPacket(
        frame_id=7,
        capture_ts_ms=123456,
        encode_ts_ms=123460,
        codec="H264",
        payload=payload,
        is_keyframe=True,
    )
    raw_packets = packetizer.packetize(encoded)
    assert raw_packets
    assembly = None
    for raw in raw_packets:
        rtp = depacketizer.parse(raw)
        assembly = depacketizer.push(rtp) or assembly
    assert assembly is not None
    assert assembly.frame_id == 7
    assert assembly.capture_ts_ms == 123456
    assert assembly.payload == payload


def test_packetize_and_depacketize_fragmented_nal() -> None:
    packetizer = RtpH264Packetizer(mtu=200, ssrc=4321)
    depacketizer = RtpH264Depacketizer()
    payload = b"\x00\x00\x00\x01\x65" + b"\xBB" * 500
    encoded = EncodedPacket(
        frame_id=11,
        capture_ts_ms=9999,
        encode_ts_ms=10001,
        codec="H264",
        payload=payload,
        is_keyframe=True,
    )
    raw_packets = packetizer.packetize(encoded)
    assert len(raw_packets) > 1
    assembly = None
    for raw in raw_packets:
        rtp = depacketizer.parse(raw)
        assembly = depacketizer.push(rtp) or assembly
    assert assembly is not None
    assert assembly.frame_id == 11
    assert assembly.payload == payload
