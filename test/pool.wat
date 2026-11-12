;; For test/pool.c: a program whose memory a host function looks into while
;; another memory is put into one block, and a sum to stop by fuel.
(module
  (import "env" "between" (func $between))
  (memory (export "memory") 1)
  ;; Calls the host in the middle, then returns the word at 0.
  (func (export "call_host") (result i32)
    (call $between)
    (i32.load (i32.const 0)))
  ;; Writes i * 2654435761 at every 1 KiB of the page, then sums them back.
  (func (export "fill") (local $i i32)
    (loop $l
      (i32.store (local.get $i) (i32.mul (local.get $i) (i32.const 2654435761)))
      (local.set $i (i32.add (local.get $i) (i32.const 1024)))
      (br_if $l (i32.lt_u (local.get $i) (i32.const 65536)))))
  (func (export "sum") (result i32) (local $i i32) (local $s i32)
    (loop $l
      (local.set $s (i32.add (local.get $s) (i32.load (local.get $i))))
      (local.set $i (i32.add (local.get $i) (i32.const 1024)))
      (br_if $l (i32.lt_u (local.get $i) (i32.const 65536))))
    (local.get $s)))
