;; A program for test/store.c: a memory of its own with a byte it counts up, room to grow, a table.
(module
  (memory (export "memory") 1 4)
  (data (i32.const 0) "\01")
  (table (export "table") 2 funcref)
  (func (export "bump") (result i32)
    (i32.store8 (i32.const 0) (i32.add (i32.load8_u (i32.const 0)) (i32.const 1)))
    (i32.load8_u (i32.const 0)))
  (func (export "grow") (param i32) (result i32) (memory.grow (local.get 0))))
