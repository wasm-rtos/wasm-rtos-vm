;; A module with no memory of its own that calls into another instance:
;; the other memory changes while this one's code runs (test/clean.c).
(module
  (import "paged" "add" (func $add (param i32)))
  (func (export "run") (param $pages i32)
    (call $add (local.get $pages))))
