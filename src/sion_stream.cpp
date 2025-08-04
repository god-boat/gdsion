#include "sion_stream.h"
#include "sion_stream_playback.h"

using namespace godot;

namespace godot {

Ref<AudioStreamPlayback> SiONStream::_instantiate_playback() const {
    Ref<SiONStreamPlayback> playback;
    playback.instantiate();
    playback->set_driver(driver);
    return playback;
}

} // namespace godot 