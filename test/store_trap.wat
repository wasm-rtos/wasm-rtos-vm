;; For test/store.c: its start function traps once its memory is made, so the instance stays in its store.
(module
  (memory 1)
  (func $start unreachable)
  (start $start))
