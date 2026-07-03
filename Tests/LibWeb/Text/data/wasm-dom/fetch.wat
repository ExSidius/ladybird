;; Source of truth for fetch.wasm (assembled by hand).
;;
;; M3 fixture: _start issues dom.fetch("../../data/wasm-dom/message.txt") — the URL
;; is parsed relative to the *document*, i.e. the test page under input/Wasm/. The
;; completion callback receives a Response handle (0 on network failure), checks
;; response_status == 200, reads the body with a deliberately tiny 4-byte buffer
;; first (retry ABI), then writes it into #out.
(module
  (import "dom" "get_element_by_id" (func $get_element_by_id (param i32 i32) (result i32)))
  (import "dom" "set_attribute" (func $set_attribute (param i32 i32 i32 i32 i32) (result i32)))
  (import "dom" "text_content_set" (func $text_content_set (param i32 i32 i32) (result i32)))
  (import "dom" "fetch" (func $fetch (param i32 i32 i32 i32) (result i32)))
  (import "dom" "response_status" (func $response_status (param i32) (result i32)))
  (import "dom" "response_read" (func $response_read (param i32 i32 i32) (result i32)))

  (table (export "__indirect_function_table") 1 funcref)
  (elem (i32.const 0) $on_fetch)
  (memory (export "memory") 1)

  (data (i32.const 0) "out")
  (data (i32.const 8) "../../data/wasm-dom/message.txt")
  (data (i32.const 48) "data-status")
  (data (i32.const 64) "ok")
  ;; scratch buffer at 256

  (func $on_fetch (param $resp i32) (param $user_data i32)
    (local $out i32) (local $len i32)
    (local.set $out (call $get_element_by_id (i32.const 0) (i32.const 3)))
    (if (local.get $resp)
      (then
        (if (i32.eq (call $response_status (local.get $resp)) (i32.const 200))
          (then (drop (call $set_attribute (local.get $out) (i32.const 48) (i32.const 11) (i32.const 64) (i32.const 2)))))
        (local.set $len (call $response_read (local.get $resp) (i32.const 256) (i32.const 4)))
        (local.set $len (call $response_read (local.get $resp) (i32.const 256) (local.get $len)))
        (drop (call $text_content_set (local.get $out) (i32.const 256) (local.get $len))))))

  (func (export "_start")
    (drop (call $fetch (i32.const 8) (i32.const 31) (i32.const 0) (i32.const 5)))))
