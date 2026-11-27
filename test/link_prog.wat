;; A program for test/link.c: its function imports are left unresolved and
;; bound late; one of them is in its table, and one is exported unbound.
(module
  (type $r (func (result i32)))
  (import "lib" "add" (func $add (param i32 i32) (result i32)))
  (import "lib" "seven" (func $seven (result i32)))
  (import "env" "twice" (func $twice (param i32) (result i32)))
  (import "env" "base" (global i64))
  (table (export "table") 1 funcref)
  (elem (i32.const 0) $seven)
  (func (export "run") (result i32) (call $add (i32.const 2) (i32.const 3)))
  (func (export "indirect") (result i32) (call_indirect (type $r) (i32.const 0)))
  (func (export "doubled") (result i32) (call $twice (i32.const 21)))
  (export "twice" (func $twice)))
