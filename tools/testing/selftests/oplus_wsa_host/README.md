# WSA host callback-model regression tests

These tests compile the **actual driver C** with host-only kernel-API replacements,
then drive registered module, platform-device, generic-netlink and module-parameter
callbacks. They do not call the driver's private helpers directly.

From the kernel repository root:

```sh
python3 tools/testing/selftests/oplus_wsa_host/run.py \
  --source net/oplus_wifi_wsa/oplus_wifismartantenna.c
```

Requirements: Python3, host Clang with AddressSanitizer/UndefinedBehaviorSanitizer,
and pthreads. No downloads, kernel build, root, real netlink/sysfs, module loading
or radio access is used. Each run uses a new retained `runs/host-*` directory;
compiler commands/identity, source/harness/runner snapshots and per-case results
are retained. Core dumps are disabled; compile timeout90s and each case20s.

Twenty cases cover registration failures and unwind, absent/unready devices,
wire command/parameter behavior, rejected inputs, failed selection/recovery,
second-device rejection, remove/rebind, controlled switch/remove races through
both mutation routes,50 repeated lifecycles, failed initial selection, missing
DT and7 resource/lookup failure variants including deferred/null results.

The threaded races block pin selection at the mocked pinctrl boundary, observe
remove attempting the mutex API, then release selection and assert devm release
occurs afterward. This is a narrow concurrency-contract check, not Linux lockdep
or proof of every schedule. The harness checks modeled allocations balance.
LeakSanitizer is disabled because the primary macOS host does not support it;
that resource accounting is not a kernel kmemleak result.

**Limits:** API structs/functions are a model, not real Linux headers or framework
code. Host ASan/UBSan do not validate ARM64 module ABI, kernel CFI/ThinLTO,
capability/namespace enforcement, real device-resource or netlink teardown,
Linux locking/scheduler contexts, DT/pinctrl validity, GPIO voltage/polarity,
RF function or hardware rollback. The modeled permission check is not a test of
real generic-netlink authorization. Compile/type checks against the actual pinned
kernel and separately authorized live allow/deny, fault and lifecycle tests are
still required. Do not substitute these tests for a recoverable device test plan.

Source is GPL-2.0-only; see the repository's COPYING file.
