# Host validation

Run all portable gates with:

```bash
bash host/build.sh
```

The runner compiles with strict warnings plus AddressSanitizer and
UndefinedBehaviorSanitizer, then executes:

- the 59-check PT-P710BT byte oracle and PackBits round-trip suite;
- model/profile tests that build every available recipe, validate geometry and
  media admission, pin PT-P710BT streams, and inspect PT-1950/P900 placement;
- completion-state tests for stale frames, ordered P710 notifications, legacy
  job status, printer errors, and idle readiness;
- deterministic fuzz-style USB descriptor parsing over 10,000 arbitrary inputs;
- source-contract guards for bounded transfer reclamation, callback-task
  dispatch, raw-output quarantine, single start, and generic public naming.

CI runs the suite with GCC and Clang. The ESP-IDF job separately builds the
public example on the pinned ESP-IDF 6.0.2 / ESP32-S3 toolchain image.

The committed `oracle_generated.h` lets the suite run offline. Regenerate it
only when the low-level command builders intentionally change; see
`host/gen_oracle.py` and [PROVENANCE.md](../docs/PROVENANCE.md).
