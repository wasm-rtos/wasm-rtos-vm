;; A program for test/kept.c: it defines the memory, with data of its own.
(module
  (memory (export "memory") 1 2)
  (data (i32.const 0) "prog")
  (func (export "first") (result i32) (i32.load8_u (i32.const 0))))
