#include "sion_stream_playback.h"
#include "sion_driver.h"
#include <godot_cpp/core/math.hpp>

using namespace godot;

namespace godot {

/* ------------------------------------------------------------------ */
/* Convenience wrappers – defer to the underscore virtuals            */
/* ------------------------------------------------------------------ */

void SiONStreamPlayback::start(double p_from_pos) {
    _start(p_from_pos);
}

void SiONStreamPlayback::stop() {
    _stop();
}

bool SiONStreamPlayback::is_playing() const {
    return _is_playing();
}

int SiONStreamPlayback::get_loop_count() const {
    return _get_loop_count();
}

double SiONStreamPlayback::get_playback_position() const {
    return _get_playback_position();
}

void SiONStreamPlayback::seek(double p_time) {
    _seek(p_time);
}

int32_t SiONStreamPlayback::mix(AudioFrame *p_buffer, float p_rate_scale, int32_t p_frames) {
    return _mix(p_buffer, p_rate_scale, p_frames);
}

double SiONStreamPlayback::get_length() const {
    return 0.0;
}

/* ------------------------------------------------------------------ */
/* Actual overrides                                                   */
/* ------------------------------------------------------------------ */

void SiONStreamPlayback::_start(double p_from_pos) {
    playing = true;
    playback_position = p_from_pos;
}

void SiONStreamPlayback::_stop() {
    playing = false;
}

bool SiONStreamPlayback::_is_playing() const {
    return playing;
}

int32_t SiONStreamPlayback::_get_loop_count() const {
    return loop_count;
}

double SiONStreamPlayback::_get_playback_position() const {
    return playback_position;
}

void SiONStreamPlayback::_seek(double p_time) {
    playback_position = p_time;
}

int32_t SiONStreamPlayback::_mix(AudioFrame *p_buffer, float /*p_rate_scale*/, int32_t p_frames) {
    if (!playing || driver == nullptr) {
        for (int i = 0; i < p_frames; ++i) {
            p_buffer[i].left = 0.0f;
            p_buffer[i].right = 0.0f;
        }
        return p_frames;
    }

    int32_t written = driver->generate_audio(p_buffer, p_frames);
    playback_position += static_cast<double>(written) / driver->get_sample_rate();
    return written;
}

} // namespace godot
