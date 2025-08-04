#ifndef SION_STREAM_PLAYBACK_H
#define SION_STREAM_PLAYBACK_H

#include <godot_cpp/classes/audio_stream_playback.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/classes/audio_frame.hpp>
#include <climits>

class SiONDriver; // global forward declaration

namespace godot {

class SiONStreamPlayback : public AudioStreamPlayback {
    GDCLASS(SiONStreamPlayback, AudioStreamPlayback);

private:
    ::SiONDriver *driver = nullptr; // Not owned.
    bool playing = false;
    double playback_position = 0.0; // seconds
    int loop_count = 0;

protected:
    static void _bind_methods() {}

public:
    SiONStreamPlayback() = default;
    void set_driver(::SiONDriver *p_driver) { driver = p_driver; }

    // Public convenience wrappers (not virtual in base – kept for local use)
    void start(double p_from_pos = 0.0);
    void stop();
    bool is_playing() const;
    int get_loop_count() const;
    double get_playback_position() const;
    void seek(double p_time);
    int mix(AudioFrame *p_buffer, float p_rate_scale, int32_t p_frames);
    double get_length() const; // always returns 0 for streaming

    // Temporary stubs for legacy push-streaming code. These mimic
    // AudioStreamGeneratorPlayback API to keep existing driver logic
    // compiling until the push path is fully removed.
    int get_frames_available() const { return INT_MAX; }
    void push_buffer(const PackedVector2Array &/*p_buffer*/) {}

    /*
     * AudioStreamPlayback virtual interface – underscore-prefixed names as in
     * Godot 4. These actually override the base implementation.
     */
    virtual void _start(double p_from_pos = 0.0) override;
    virtual void _stop() override;
    virtual bool _is_playing() const override;
    virtual int _get_loop_count() const override;
    virtual double _get_playback_position() const override;
    virtual void _seek(double p_time) override;
    virtual int32_t _mix(AudioFrame *p_buffer, float p_rate_scale, int32_t p_frames) override;
    // _get_length() is not virtual in base; provide helper only via wrapper
};

#ifdef OLD_COMMENT_PLACEHOLDER
#endif

} // namespace godot

#endif // SION_STREAM_PLAYBACK_H
