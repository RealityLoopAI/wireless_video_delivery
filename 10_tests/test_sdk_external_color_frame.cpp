#include <libobsensor/hpp/Frame.hpp>
#include <cassert>
#include <memory>
#include <vector>

int main() {
    bool released = false;
    auto payload = std::make_shared<std::vector<uint8_t>>(1024, 0);
    auto *data = payload->data();
    auto frame = ob::FrameHelper::createFrameFromBuffer(
        OB_FORMAT_MJPG, 1920, 1080, data, static_cast<uint32_t>(payload->size()),
        [payload, &released](void *, void *) { released = true; }, nullptr);
    auto color = std::make_shared<ob::ColorFrame>(*frame);
    assert(color->data() == data);
    assert(color->dataSize() == 1024);
    assert(color->width() == 1920 && color->height() == 1080);
    frame.reset();
    assert(!released);
    color.reset();
    assert(released);
}
