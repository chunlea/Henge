# Guest shutdown and restart

PSCI `SYSTEM_OFF` and `SYSTEM_RESET` stop the whole machine. `CPU_OFF` only
parks one CPU until a later `CPU_ON`. A parked CPU waits on `wake_cv`, so setting
`halted`, an exit request, and an ARM event alone cannot release it. In
particular, CPU 0 may be parked when a secondary requests shutdown.

`hv_machine_halt` publishes a terminal reason and wakes every CPU, including
those parked in `CPU_OFF`. The halt reason is atomic. The per-CPU wake lock
serializes it with `CPU_ON` so a concurrent reset cannot clear shutdown.
`CPU_ON` also abandons its wait when the machine stops. A later host cleanup
request preserves the guest's original terminal reason.

For the asynchronous app runner, CPU 0 joins the secondary CPU threads and
stops the periodic kicker, finishes submitted block requests and stops audio
before flushing disks and calling the exit callback. Guest shutdown, restart,
host stop, panic and execution failures remain distinguishable; a generic
dispatcher `HALT` is not sufficient to identify a normal shutdown.

The native bridge stays in a transitional state until network workers, GPU
work and device callbacks have drained, the disks have closed, and the old
board, guest RAM and translation metadata have been released. UI readers are
excluded during teardown, and boot display snapshots own their pixel copies.
The bridge then publishes the stopped state and permits another boot. The
app's borrowed JIT mapping survives teardown, so the next boot reuses it
without repeating the debugger handoff. Failed normal boots also release
their partial machine and allow retry.

The session view removes guest input controls, releases captured input, restores
normal device sleep and shows a terminal message. `BootDisplay` keeps watching
the VM state after GPU rendering begins, then removes the last rendered frame
on both the device and an external display. The stopped card offers **Start
Mac** and **Close**. A guest restart request automatically starts the next boot;
closing the session and opening a Mac again also works in the same app process.
Each successful boot increments a generation counter so the display discards
the previous boot's frame and watches for the new GPU output.

This applies to normal boots of installed guests. The installer has separate
long-lived USB restore workers and retains its existing reopen-the-app
restriction; this change does not release a board still owned by those workers.

## Regression checks

On an Apple Silicon Mac:

```sh
make build/test_shutdown build/test_exc build/test_devices
ulimit -s 65520
build/test_shutdown
build/test_exc
build/test_devices
```

`test_shutdown` executes guest HVC instructions with other CPUs already parked,
including CPU 0. It covers shutdown and reset from CPU 0 and a secondary, no
further guest instructions after a power request, preservation of the terminal
reason during host cleanup, and cancellation of pending CPU_ON calls. Waits
are bounded, with cleanup to release threads even when the regression fails.

`make test-vm-lifecycle` additionally needs the app's libslirp development
dependencies. It links the production native bridge and engine, supplies a
test JIT mapping, and uses a tiny synthetic ROM. It covers guest shutdown,
restart, host stop, failed-boot recovery and concurrent UI reads. It needs no
Apple ROM or guest disk.

`make build/test_pvg_lifecycle` builds a Metal lifecycle test for Apple Silicon.
Run `build/test_pvg_lifecycle` to check that teardown drains pending GPU work
before freeing the board. This test does not cover guest image rendering.

## Validation

Tested: On iPhone Duo running with iOS 27.3 Beta 1, the guest now shuts down and
restarts normally from inside macOS.
