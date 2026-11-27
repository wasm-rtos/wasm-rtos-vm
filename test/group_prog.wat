;; A program for test/group.c: run calls the library, which calls tick
;; here, so a call stopped by fuel stands in both modules; a host function
;; is in its table.
(module
  (type $h (func (result i32)))
  (import "lib" "work" (func $work (param i32) (result i32)))
  (import "env" "nine" (func $nine (result i32)))
  (import "env" "mem" (memory 1 1))
  (table 1 funcref)
  (elem (i32.const 0) $nine)
  (func (export "tick") (param $i i32) (result i32)
    (i32.store (i32.const 0) (local.get $i))
    (i32.mul (local.get $i) (i32.const 2)))
  (func (export "run") (param $n i32) (result i32)
    (i32.add (i32.add (call $work (local.get $n)) (call_indirect (type $h) (i32.const 0)))
             (i32.load (i32.const 0)))))
