// New file header 
#ifndef SION_STREAM_H
#define SION_STREAM_H

#include <godot_cpp/classes/audio_stream.hpp>
// Forward declarations to avoid circular dependencies.
class SiONDriver;
namespace godot { class SiONStreamPlayback; }

namespace godot {

class SiONStream : public AudioStream {
    GDCLASS(SiONStream, AudioStream);

private:
    ::SiONDriver *driver = nullptr; // Not owned.

protected:
    static void _bind_methods() {}

public:
    SiONStream() = default;
    void set_driver(::SiONDriver *p_driver) { driver = p_driver; }

    // AudioStream virtual overrides (underscore-prefixed)
    virtual Ref<AudioStreamPlayback> _instantiate_playback() const override;
    virtual String _get_stream_name() const override { return "SiONStream"; }
    virtual bool _is_monophonic() const override { return false; }
    virtual double _get_length() const override { return 0; }
    // Keeping recorder_daw_info optional – not overriding as not present in base
};

} // namespace godot

#endif // SION_STREAM_H 