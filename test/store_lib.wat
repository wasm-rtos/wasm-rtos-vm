;; A library for test/store.c: it imports the program's memory and reads the byte the program counts.
(module
  (import "env" "memory" (memory 1))
  (func (export "peek") (result i32) (i32.load8_u (i32.const 0))))
