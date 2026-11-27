;; A library for test/group.c: work calls back into the program that
;; imports it, and keeps its sum in a global and a memory the host shares.
(module
  (import "prog" "tick" (func $tick (param i32) (result i32)))
  (import "env" "mem" (memory 1 1))
  (import "env" "acc" (global $acc (mut i32)))
  (global $calls (mut i32) (i32.const 0))
  (func (export "work") (param $n i32) (result i32) (local $i i32)
    (block (loop
      (br_if 1 (i32.ge_u (local.get $i) (local.get $n)))
      (global.set $acc (i32.add (global.get $acc) (call $tick (local.get $i))))
      (global.set $calls (i32.add (global.get $calls) (i32.const 1)))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br 0)))
    (i32.add (global.get $acc) (i32.sub (global.get $calls) (local.get $n)))))
