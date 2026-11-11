;; Paged memory: a memory that grows past what the heap has in one block
;; goes on paged; values written before and after must read back, also
;; unaligned ones across a frame boundary.
(module
  (memory (export "memory") 1)
  ;; Writes i * 2654435761 at every 4 KiB step and some odd places, then sums them back.
  (func (export "fill") (param $pages i32) (result i32)
    (local $i i32) (local $end i32)
    (local.set $end (i32.mul (local.get $pages) (i32.const 65536)))
    (loop $l
      (i32.store (local.get $i) (i32.mul (local.get $i) (i32.const 2654435761)))
      (i32.store offset=4095 (local.get $i) (i32.const 0x01020304))
      (local.set $i (i32.add (local.get $i) (i32.const 4096)))
      (br_if $l (i32.lt_u (i32.add (local.get $i) (i32.const 4100)) (local.get $end))))
    (local.get $i))
  (func (export "sum") (param $pages i32) (result i32)
    (local $i i32) (local $end i32) (local $s i32)
    (local.set $end (i32.mul (local.get $pages) (i32.const 65536)))
    (loop $l
      (local.set $s (i32.add (local.get $s) (i32.load (local.get $i))))
      (local.set $s (i32.xor (local.get $s) (i32.load offset=4095 (local.get $i))))
      (local.set $i (i32.add (local.get $i) (i32.const 4096)))
      (br_if $l (i32.lt_u (i32.add (local.get $i) (i32.const 4100)) (local.get $end))))
    (local.get $s))
  (func (export "grow") (param $pages i32) (result i32)
    (memory.grow (local.get $pages)))
  (func (export "copy") (param $to i32) (param $from i32) (param $n i32)
    (memory.copy (local.get $to) (local.get $from) (local.get $n)))
  (func (export "peek") (param $at i32) (result i32)
    (i32.load (local.get $at))))
