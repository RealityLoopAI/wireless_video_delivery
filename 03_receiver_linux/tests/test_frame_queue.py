from wireless_video.frame_queue import FrameQueue


def test_push_latest_drops_oldest() -> None:
    queue = FrameQueue[int](2)
    queue.push_latest(1)
    queue.push_latest(2)
    queue.push_latest(3)

    assert queue.drops == 1
    assert queue.pop(10) == 2
    assert queue.pop(10) == 3
    assert queue.pop(10) is None

