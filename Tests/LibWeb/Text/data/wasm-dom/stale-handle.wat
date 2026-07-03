;; Source of truth for stale-handle.wasm (assembled by hand).
;;
;; Using a handle after dom.release must trap, aborting _start. The append that
;; happened before the trap stays in the DOM (host calls are not transactional);
;; the append after the trap point must never happen.
(module
  (import "dom" "get_element_by_id" (func $get_element_by_id (param i32 i32) (result i32)))
  (import "dom" "create_element" (func $create_element (param i32 i32) (result i32)))
  (import "dom" "append_child" (func $append_child (param i32 i32) (result i32)))
  (import "dom" "release" (func $release (param i32)))

  (memory (export "memory") 1)

  (data (i32.const 0) "out")
  (data (i32.const 8) "p")
  (data (i32.const 16) "q")

  (func (export "_start")
    (local $out i32) (local $p i32)

    (local.set $out (call $get_element_by_id (i32.const 0) (i32.const 3)))
    (local.set $p (call $create_element (i32.const 8) (i32.const 1)))
    (drop (call $append_child (local.get $out) (local.get $p)))

    (call $release (local.get $p))
    ;; Trap here: $p is stale.
    (drop (call $append_child (local.get $out) (local.get $p)))

    ;; Never reached.
    (local.set $p (call $create_element (i32.const 16) (i32.const 1)))
    (drop (call $append_child (local.get $out) (local.get $p)))))
