(module
  ;; A loop with no calls, as a task's main that never returns: only moving
  ;; the stopped function to its records makes it fast.
  (func (export "sum") (param $n i32) (result i32)
    (local $i i32) (local $s i32)
    (block $done
      (loop $next
        (br_if $done (i32.ge_u (local.get $i) (local.get $n)))
        (local.set $s (i32.add (local.get $s) (i32.mul (local.get $i) (local.get $i))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $next)))
    (local.get $s))
)
