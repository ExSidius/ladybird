;; Source of truth for hello-dom.wasm (assembled by hand; any wat assembler,
;; e.g. `wat2wasm hello-dom.wat -o hello-dom.wasm`, regenerates it).
;;
;; M0 fixture: builds <p id="made-by-wasm">hello from wasm</p> inside #out
;; using only the "dom" host interface. No JavaScript is involved.
(module
  (import "dom" "get_element_by_id" (func $get_element_by_id (param i32 i32) (result i32)))
  (import "dom" "create_element" (func $create_element (param i32 i32) (result i32)))
  (import "dom" "create_text_node" (func $create_text_node (param i32 i32) (result i32)))
  (import "dom" "append_child" (func $append_child (param i32 i32) (result i32)))
  (import "dom" "set_attribute" (func $set_attribute (param i32 i32 i32 i32 i32) (result i32)))
  (import "dom" "release" (func $release (param i32)))

  (memory (export "memory") 1)

  (data (i32.const 0) "out")
  (data (i32.const 16) "p")
  (data (i32.const 32) "hello from wasm")
  (data (i32.const 64) "id")
  (data (i32.const 80) "made-by-wasm")

  (func (export "_start")
    (local $out i32) (local $p i32) (local $text i32)

    (local.set $out (call $get_element_by_id (i32.const 0) (i32.const 3)))
    (local.set $p (call $create_element (i32.const 16) (i32.const 1)))
    (drop (call $set_attribute (local.get $p) (i32.const 64) (i32.const 2) (i32.const 80) (i32.const 12)))
    (local.set $text (call $create_text_node (i32.const 32) (i32.const 15)))
    (drop (call $append_child (local.get $p) (local.get $text)))
    (drop (call $append_child (local.get $out) (local.get $p)))

    (call $release (local.get $text))
    (call $release (local.get $p))
    (call $release (local.get $out))))
