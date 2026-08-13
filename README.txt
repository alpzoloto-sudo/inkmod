InkMOD release/runtime parity fix

Problem:
developer works, while release can behave differently (for example on EPUB).

Cause:
developer defines ENABLE_SERIAL_LOG, release did not.
That define is used not only by LOG_* macros but also by real hardware/runtime
code under #ifdef ENABLE_SERIAL_LOG (USB CDC setup/teardown and other paths).

Fix:
release now also defines:
  -DENABLE_SERIAL_LOG
  -DLOG_LEVEL=-1

LOG_LEVEL=-1 means LOG_ERR/LOG_INF/LOG_DBG still compile to nothing.
So:
- release stays silent
- no serial log spam
- USB flashing remains available
- developer/release execute the same ENABLE_SERIAL_LOG-gated runtime paths

Build:
  pio run -e release -t upload

Diagnostic:
If the problematic EPUB now behaves like developer, the bug was confirmed as
a build-configuration divergence rather than EPUB parsing or -Os optimization.
