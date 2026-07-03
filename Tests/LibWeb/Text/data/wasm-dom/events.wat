;; Source of truth for events.wasm (assembled by hand).
;;
;; M2 fixture: registers a click listener on #btn via the exported funcref table.
;; Each click appends <li data-user="yes">click</li> to #out — the text is the
;; event type read back through dom.event_type, and data-user proves the
;; user_data value (7) reached the callback.
(module
  (import "dom" "get_element_by_id" (func $get_element_by_id (param i32 i32) (result i32)))
  (import "dom" "create_element" (func $create_element (param i32 i32) (result i32)))
  (import "dom" "append_child" (func $append_child (param i32 i32) (result i32)))
  (import "dom" "add_event_listener" (func $add_event_listener (param i32 i32 i32 i32 i32) (result i32)))
  (import "dom" "set_attribute" (func $set_attribute (param i32 i32 i32 i32 i32) (result i32)))
  (import "dom" "event_type" (func $event_type (param i32 i32 i32) (result i32)))
  (import "dom" "text_content_set" (func $text_content_set (param i32 i32 i32) (result i32)))

  (table (export "__indirect_function_table") 1 funcref)
  (elem (i32.const 0) $on_click)
  (memory (export "memory") 1)

  (data (i32.const 0) "out")
  (data (i32.const 8) "btn")
  (data (i32.const 16) "click")
  (data (i32.const 24) "li")
  (data (i32.const 32) "data-user")
  (data (i32.const 48) "yes")
  ;; scratch buffer at 256

  (func $on_click (param $event i32) (param $user_data i32)
    (local $len i32) (local $li i32)

    (local.set $len (call $event_type (local.get $event) (i32.const 256) (i32.const 64)))
    (local.set $li (call $create_element (i32.const 24) (i32.const 2)))
    (drop (call $text_content_set (local.get $li) (i32.const 256) (local.get $len)))

    (if (i32.eq (local.get $user_data) (i32.const 7))
      (then (drop (call $set_attribute (local.get $li) (i32.const 32) (i32.const 9) (i32.const 48) (i32.const 3)))))

    (drop (call $append_child (call $get_element_by_id (i32.const 0) (i32.const 3)) (local.get $li))))

  (func (export "_start")
    (drop (call $add_event_listener
      (call $get_element_by_id (i32.const 8) (i32.const 3))
      (i32.const 16) (i32.const 5)  ;; "click"
      (i32.const 0)                 ;; table index of $on_click
      (i32.const 7)))))             ;; user_data
