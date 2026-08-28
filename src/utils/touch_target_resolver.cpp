/***************************************************/
/* Touch target resolution for Pooly UI roots      */
/* Provided under MIT                              */
/***************************************************/

#include "touch_target_resolver.h"

#include <godot_cpp/classes/base_button.hpp>
#include <godot_cpp/classes/canvas_item.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/range.hpp>
#include <godot_cpp/classes/text_edit.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/transform2d.hpp>

using namespace godot;

void TouchTargetResolver::_bind_methods() {
	ClassDB::bind_method(D_METHOD("configure_roots", "roots"), &TouchTargetResolver::configure_roots);
	ClassDB::bind_method(D_METHOD("classify", "position"), &TouchTargetResolver::classify);
	ClassDB::bind_method(D_METHOD("pick", "position"), &TouchTargetResolver::pick);
	ClassDB::bind_method(D_METHOD("get_last_visit_count"), &TouchTargetResolver::get_last_visit_count);

	BIND_ENUM_CONSTANT(PASS_THROUGH);
	BIND_ENUM_CONSTANT(UI_BLOCKING);
	BIND_ENUM_CONSTANT(CAPTURED);
}

void TouchTargetResolver::configure_roots(const Array &p_roots) {
	ERR_FAIL_COND_MSG(p_roots.is_empty(), "TouchTargetResolver requires at least one UI root.");

	for (int root_index = 0; root_index < p_roots.size(); root_index++) {
		Object *root_object = p_roots[root_index];
		Node *root = Object::cast_to<Node>(root_object);
		ERR_FAIL_NULL_MSG(root, "TouchTargetResolver roots must be Nodes.");
		ERR_FAIL_COND_MSG(
			Object::cast_to<CanvasItem>(root) == nullptr && Object::cast_to<CanvasLayer>(root) == nullptr,
			"TouchTargetResolver roots must be CanvasItems or CanvasLayers."
		);
		ERR_FAIL_COND_MSG(!root->is_inside_tree(), "TouchTargetResolver roots must be inside the SceneTree.");
	}

	roots = p_roots.duplicate();
}

// DELIBERATE DIVERGENCE FROM THE ENGINE PICKER -- do not "fix" this.
//
// get_child_count()/get_child() default p_include_internal to FALSE in godot-cpp and TRUE
// in the engine, so Viewport's picker walks internal children and this one does not.
//
// The reason is scope, NOT speed. Measured both ways, including internal children costs
// about 2 extra visits per pick (337.6 -> 339.6 in the browser state) because the internal
// scrollbars are invisible and the recursion stops at is_visible() without descending. The
// 573-node difference between a 3428- and a 4001-node tree is tree size, not per-pick work;
// do not repeat that number as a cost.
//
// Pooly does not use ScrollContainer's built-in scrollbars, and CustomScrollContainer adds
// its drag handle INTERNAL_MODE_BACK, so what is behind that door is chrome the gesture
// layer is meant to ignore. Skipping it is the point.
//
// Note this is NOT what makes the scroll handle mismatch the engine picker: the handle is
// also is_set_as_top_level(), and that alone hides it (measured -- including internal
// children left all 25 mismatches in place). See the top_level accepted deviation in
// tools/touch_target/README.md.
Control *TouchTargetResolver::_find_control_in_children(Node *p_parent, const Vector2 &p_position) const {
	for (int child_index = p_parent->get_child_count() - 1; child_index >= 0; child_index--) {
		Node *child = p_parent->get_child(child_index);
		if (Object::cast_to<CanvasLayer>(child) != nullptr) {
			continue;
		}

		CanvasItem *canvas_item = Object::cast_to<CanvasItem>(child);
		if (canvas_item != nullptr) {
			Control *hit = _enter_subtree(canvas_item, p_position);
			if (hit != nullptr) {
				return hit;
			}
			continue;
		}

		Control *hit = _find_control_in_children(child, p_position);
		if (hit != nullptr) {
			return hit;
		}
	}

	return nullptr;
}

// Establishes what the recursion below then maintains: this item is visible all the way
// up, and its global transform is known. is_visible_in_tree() is a maintained flag and
// get_global_transform() is memoized, so neither is expensive -- they are hoisted here
// because the recursion can carry both answers forward, not because they are slow.
Control *TouchTargetResolver::_enter_subtree(CanvasItem *p_item, const Vector2 &p_position) const {
	if (!p_item->is_visible_in_tree()) {
		return nullptr;
	}

	return _find_control_at(p_item, p_position, p_item->get_global_transform_with_canvas());
}

Control *TouchTargetResolver::_find_control_at(CanvasItem *p_item, const Vector2 &p_position, const Transform2D &p_global_transform) const {
	last_visit_count++;

	// Local visibility only, matching the engine picker. Every caller has already
	// established that this item's ancestors are visible, so the tree-wide form would
	// re-derive what the caller already knows.
	if (!p_item->is_visible()) {
		return nullptr;
	}

	if (p_global_transform.determinant() == 0.0) {
		return nullptr;
	}

	Vector2 local_position = p_global_transform.affine_inverse().xform(p_position);
	Control *control = Object::cast_to<Control>(p_item);
	// Control::has_point() is deliberately NOT bound to ClassDB -- only the _has_point
	// virtual is -- so an extension cannot call it, and cannot reach a C++ override such
	// as TextureButton's click mask at all. This rect test is what Control::has_point()
	// itself falls back to when nothing overrides it, which is every Control in this app.
	// tools/touch_target scans for controls that would break that assumption and its
	// parity sweep fails if the picks ever diverge.
	bool contains_position = control != nullptr && Rect2(Vector2(), control->get_size()).has_point(local_position);
	bool inspect_children = control == nullptr || !control->is_clipping_contents() || contains_position;

	if (inspect_children) {
		// Internal children are skipped on purpose; see the note above
		// _find_control_in_children(). These defaults are the divergence, not a slip.
		for (int child_index = p_item->get_child_count() - 1; child_index >= 0; child_index--) {
			CanvasItem *child = Object::cast_to<CanvasItem>(p_item->get_child(child_index));
			if (child == nullptr || child->is_set_as_top_level()) {
				continue;
			}

			// Composing the parent's global transform with the child's local one is
			// exactly what get_global_transform_with_canvas() would return for the
			// child. It holds only because top-level children -- the one case that
			// breaks the chain by design -- are skipped just above.
			//
			// Note this is a like-for-like swap of one engine call for another
			// (get_transform for get_global_transform_with_canvas), so it is not a
			// throughput win; it is here because it matches the engine picker and
			// stays correct when the engine's cached global transform is invalidated.
			Control *hit = _find_control_at(child, p_position, p_global_transform * child->get_transform());
			if (hit != nullptr) {
				return hit;
			}
		}
	}

	if (control != nullptr && contains_position && control->get_mouse_filter_with_override() != Control::MOUSE_FILTER_IGNORE) {
		return control;
	}

	return nullptr;
}

Dictionary TouchTargetResolver::_make_result(Classification p_classification, Control *p_isolating) const {
	Dictionary result;
	result["classification"] = p_classification;
	result["isolating"] = p_isolating != nullptr ? Variant(p_isolating) : Variant();
	return result;
}

Control *TouchTargetResolver::pick(const Vector2 &p_position) const {
	ERR_FAIL_COND_V_MSG(roots.is_empty(), nullptr, "TouchTargetResolver roots have not been configured.");

	last_visit_count = 0;

	Control *hit = nullptr;
	for (int root_index = 0; root_index < roots.size(); root_index++) {
		Object *root_object = roots[root_index];
		Node *root = Object::cast_to<Node>(root_object);
		ERR_FAIL_NULL_V_MSG(root, nullptr, "A configured TouchTargetResolver root no longer exists.");

		CanvasLayer *canvas_layer = Object::cast_to<CanvasLayer>(root);
		if (canvas_layer != nullptr) {
			if (canvas_layer->is_visible()) {
				hit = _find_control_in_children(canvas_layer, p_position);
			}
		} else {
			CanvasItem *canvas_item = Object::cast_to<CanvasItem>(root);
			if (canvas_item != nullptr) {
				hit = _enter_subtree(canvas_item, p_position);
			}
		}

		if (hit != nullptr) {
			break;
		}
	}

	return hit;
}

Dictionary TouchTargetResolver::classify(const Vector2 &p_position) const {
	ERR_FAIL_COND_V_MSG(roots.is_empty(), Dictionary(), "TouchTargetResolver roots have not been configured.");

	Control *hit = pick(p_position);
	if (hit == nullptr) {
		return _make_result(PASS_THROUGH);
	}

	Control *isolating = nullptr;
	bool has_blocking_control = false;
	Node *current = hit;
	while (current != nullptr) {
		Control *current_control = Object::cast_to<Control>(current);
		if (current_control == nullptr) {
			break;
		}

		if (current_control->is_visible_in_tree() && current_control->get_mouse_filter() != Control::MOUSE_FILTER_IGNORE) {
			if (current_control->is_in_group("touch_capturing")) {
				return _make_result(CAPTURED);
			}
			if (isolating == nullptr && current_control->is_in_group("drag_isolating")) {
				isolating = current_control;
			}
			if (
				Object::cast_to<BaseButton>(current_control) != nullptr ||
				Object::cast_to<Range>(current_control) != nullptr ||
				Object::cast_to<LineEdit>(current_control) != nullptr ||
				Object::cast_to<TextEdit>(current_control) != nullptr
			) {
				has_blocking_control = true;
			}
		}

		current = current_control->get_parent();
	}

	if (isolating != nullptr) {
		return _make_result(PASS_THROUGH, isolating);
	}
	if (has_blocking_control) {
		return _make_result(UI_BLOCKING);
	}
	return _make_result(PASS_THROUGH);
}
