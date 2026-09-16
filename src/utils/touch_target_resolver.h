/***************************************************/
/* Touch target resolution for Pooly UI roots      */
/* Provided under MIT                              */
/***************************************************/

#ifndef TOUCH_TARGET_RESOLVER_H
#define TOUCH_TARGET_RESOLVER_H

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/binder_common.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/transform2d.hpp>
#include <godot_cpp/variant/vector2.hpp>

namespace godot {
class CanvasItem;
class Control;
class Node;
}

using namespace godot;

class TouchTargetResolver : public RefCounted {
	GDCLASS(TouchTargetResolver, RefCounted)

public:
	enum Classification {
		PASS_THROUGH,
		UI_BLOCKING,
		CAPTURED,
	};

private:
	Array roots;
	// Nodes reached by the last pick(). Diagnostic only: tools/touch_target compares it
	// against the engine picker's visit count to tell "we walk more of the tree" apart
	// from "we pay more per node". Incrementing it costs no engine call.
	mutable int64_t last_visit_count = 0;

	// p_global_transform is p_item's own global-with-canvas transform, composed by the
	// caller rather than re-fetched here. This mirrors the engine picker's shape and
	// keeps the result correct even in the frames where the engine's cached global
	// transform has been invalidated by an ancestor moving.
	Control *_find_control_at(CanvasItem *p_item, const Vector2 &p_position, const Transform2D &p_global_transform) const;
	Control *_find_control_in_children(Node *p_parent, const Vector2 &p_position) const;
	bool _control_contains_point(Control *p_control, const Vector2 &p_local_position) const;
	// Seeds a subtree walk: establishes visibility up the chain and the starting transform.
	Control *_enter_subtree(CanvasItem *p_item, const Vector2 &p_position) const;
	Dictionary _make_result(Classification p_classification, Control *p_isolating = nullptr) const;

protected:
	static void _bind_methods();

public:
	// Roots are searched from topmost to bottommost. The resolver deliberately covers
	// Pooly's rectangular Control tree rather than Viewport's full private GUI picker.
	void configure_roots(const Array &p_roots);
	Dictionary classify(const Vector2 &p_position) const;
	// The pick half of classify(), without the ancestor walk. Exposed so the bench can
	// price the two halves separately against the engine picker.
	Control *pick(const Vector2 &p_position) const;
	int64_t get_last_visit_count() const { return last_visit_count; }

	TouchTargetResolver() {}
	~TouchTargetResolver() {}
};

VARIANT_ENUM_CAST(TouchTargetResolver::Classification);

#endif // TOUCH_TARGET_RESOLVER_H
