from wireless_video.config import DisplayConfig, NetworkRxConfig, ReceiverConfig, RuntimeConfig
from wireless_video.models import FrameAssembly
from wireless_video.receiver_app import ReceiverService


def _receiver_cfg() -> ReceiverConfig:
    return ReceiverConfig(
        network=NetworkRxConfig(jitter_ms=0),
        runtime=RuntimeConfig(),
        display=DisplayConfig(show_stats=False),
    )


def test_heap_handles_same_arrival_timestamp_without_typeerror() -> None:
    service = ReceiverService(_receiver_cfg())
    first = FrameAssembly(
        frame_id=1,
        capture_ts_ms=1000,
        payload=b"a",
        is_keyframe=False,
        arrival_ts_ms=123456,
        timestamp=1,
        seq_end=1,
    )
    second = FrameAssembly(
        frame_id=2,
        capture_ts_ms=1001,
        payload=b"b",
        is_keyframe=False,
        arrival_ts_ms=123456,
        timestamp=2,
        seq_end=2,
    )

    service._push_assembly(first)
    service._push_assembly(second)

    popped_first = service._pop_due_assembly(now_ms=123456, jitter_ms=0)
    popped_second = service._pop_due_assembly(now_ms=123456, jitter_ms=0)

    assert popped_first is not None
    assert popped_second is not None
    assert popped_first.frame_id == 1
    assert popped_second.frame_id == 2
