;; Source of truth for timeout.wasm (assembled by hand).
;;
;; M3 fixture: _start schedules dom.set_timeout(1ms); the callback (delivered as
;; an ordinary event-loop turn) appends <li data-user="yes">tick</li> to #out.
;; user_data 42 proves the companion value round-trips.
(module
  (import "dom" "get_element_by_id" (func $get_element_by_id (param i32 i32) (result i32)))
  (import "dom" "create_element" (func $create_element (param i32 i32) (result i32)))
  (import "dom" "append_child" (func $append_child (param i32 i32) (result i32)))
  (import "dom" "set_attribute" (func $set_attribute (param i32 i32 i32 i32 i32) (result i32)))
  (import "dom" "text_content_set" (func $text_content_set (param i32 i32 i32) (result i32)))
  (import "dom" "set_timeout" (func $set_timeout (param i32 i32 i32) (result i32)))

  (table (export "__indirect_function_table") 1 funcref)
  (elem (i32.const 0) $on_timeout)
  (memory (export "memory") 1)

  (data (i32.const 0) "out")
  (data (i32.const 8) "li")
  (data (i32.const 16) "tick")
  (data (i32.const 32) "data-user")
  (data (i32.const 48) "yes")

  (func $on_timeout (param $arg i32) (param $user_data i32)
    (local $li i32)
    (local.set $li (call $create_element (i32.const 8) (i32.const 2)))
    (drop (call $text_content_set (local.get $li) (i32.const 16) (i32.const 4)))
    (if (i32.eq (local.get $user_data) (i32.const 42))
      (then (drop (call $set_attribute (local.get $li) (i32.const 32) (i32.const 9) (i32.const 48) (i32.const 3)))))
    (drop (call $append_child (call $get_element_by_id (i32.const 0) (i32.const 3)) (local.get $li))))

  (func (export "_start")
    (drop (call $set_timeout (i32.const 1) (i32.const 0) (i32.const 42)))))
