;; A library for test/link.c: what a linker of modules reads (exports of
;; each kind) and the functions it binds into the program.
(module
  (func (export "add") (param i32 i32) (result i32) (i32.add (local.get 0) (local.get 1)))
  (func (export "seven") (result i32) (i32.const 7))
  (memory (export "mem") 1 2)
  (global (export "count") (mut i32) (i32.const 5))
  (table (export "table") 2 funcref))
