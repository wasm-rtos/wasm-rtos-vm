;; A library for test/kept.c: it imports the program's memory and writes data
;; of its own into it; bump changes that data, as a running program would.
(module
  (import "env" "memory" (memory 1))
  (data (i32.const 16) "L")
  (func (export "bump") (result i32)
    (i32.store8 (i32.const 16) (i32.add (i32.load8_u (i32.const 16)) (i32.const 1)))
    (i32.load8_u (i32.const 16))))
