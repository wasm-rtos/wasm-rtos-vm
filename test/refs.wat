;; Snapshots with references on the stack (test/refs.c): a funcref in a
;; local and another under a loop, both alive when fuel runs out.
(module
  (type $r (func (result i32)))
  (table 2 funcref)
  (func $seven (result i32) (i32.const 7))
  (func $five (result i32) (i32.const 5))
  (elem declare func $seven $five)
  (func (export "run") (result i32) (local $f funcref) (local $i i32)
    (local.set $f (ref.func $seven))
    (i32.const 1)
    (ref.func $five)
    (block (loop
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br_if 0 (i32.lt_u (local.get $i) (i32.const 1000)))))
    (table.set)
    (table.set (i32.const 0) (local.get $f))
    (i32.add (call_indirect (type $r) (i32.const 0)) (call_indirect (type $r) (i32.const 1)))))
