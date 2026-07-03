;; Source of truth for strings.wasm (assembled by hand; any wat assembler regenerates it).
;;
;; M1 fixture: exercises host->guest string returns and the caller-buffer retry ABI.
;;  - reads #src's data-msg with a 4-byte buffer (gets 4 bytes + the full length),
;;    puts the truncated bytes into #trunc
;;  - retries with the returned length, puts the full value into #dst
;;  - copies #src's textContent into #copy
;;  - looks up an absent attribute (returns -1) and records it as
;;    data-missing="yes" on #trunc
(module
  (import "dom" "get_element_by_id" (func $get_element_by_id (param i32 i32) (result i32)))
  (import "dom" "create_text_node" (func $create_text_node (param i32 i32) (result i32)))
  (import "dom" "append_child" (func $append_child (param i32 i32) (result i32)))
  (import "dom" "get_attribute" (func $get_attribute (param i32 i32 i32 i32 i32) (result i32)))
  (import "dom" "set_attribute" (func $set_attribute (param i32 i32 i32 i32 i32) (result i32)))
  (import "dom" "text_content_get" (func $text_content_get (param i32 i32 i32) (result i32)))
  (import "dom" "text_content_set" (func $text_content_set (param i32 i32 i32) (result i32)))

  (memory (export "memory") 1)

  (data (i32.const 0) "src")
  (data (i32.const 8) "dst")
  (data (i32.const 16) "trunc")
  (data (i32.const 24) "copy")
  (data (i32.const 32) "data-msg")
  (data (i32.const 48) "missing")
  (data (i32.const 64) "data-missing")
  (data (i32.const 80) "yes")
  ;; scratch buffer at 256

  (func (export "_start")
    (local $src i32) (local $dst i32) (local $trunc i32) (local $copy i32) (local $len i32) (local $tmp i32)

    (local.set $src (call $get_element_by_id (i32.const 0) (i32.const 3)))
    (local.set $dst (call $get_element_by_id (i32.const 8) (i32.const 3)))
    (local.set $trunc (call $get_element_by_id (i32.const 16) (i32.const 5)))
    (local.set $copy (call $get_element_by_id (i32.const 24) (i32.const 4)))

    ;; Small-cap read: 4 bytes written, full length returned.
    (local.set $len (call $get_attribute (local.get $src) (i32.const 32) (i32.const 8) (i32.const 256) (i32.const 4)))
    (drop (call $text_content_set (local.get $trunc) (i32.const 256) (i32.const 4)))

    ;; Retry with adequate capacity.
    (local.set $len (call $get_attribute (local.get $src) (i32.const 32) (i32.const 8) (i32.const 256) (local.get $len)))
    (drop (call $text_content_set (local.get $dst) (i32.const 256) (local.get $len)))

    ;; textContent round-trip.
    (local.set $len (call $text_content_get (local.get $src) (i32.const 256) (i32.const 128)))
    (drop (call $text_content_set (local.get $copy) (i32.const 256) (local.get $len)))

    ;; Absent attribute -> -1.
    (if (i32.eq (call $get_attribute (local.get $src) (i32.const 48) (i32.const 7) (i32.const 256) (i32.const 128)) (i32.const -1))
      (then (drop (call $set_attribute (local.get $trunc) (i32.const 64) (i32.const 12) (i32.const 80) (i32.const 3)))))))
