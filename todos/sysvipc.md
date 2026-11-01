# System V IPC

Turn on System V IPC (`CONFIG_SYSVIPC`) without breaking Mobvoi's stock audio modules.

Status (2026-11-01):

- **Done in the kernel.** This branch adds two commits, listed [below](#the-fix). With them, `semget()` and the rest of SysV IPC work, and the 25 stock audio modules keep their ABI: every CRC they check still matches (`modcrc.py`: 493 match, 0 mismatch, 0 missing), and every `task_struct` member they can reach keeps its offset.
- **Trial build 023** (`#23-fc9ef77`) has run from `recovery` since 2026-11-01 01:04 UTC. It was confirmed healthy 164 s after boot. SysV IPC works: the test program passes as `ceres`, and at boot mkcal, loaded by asteroid-launcher, created its lock and, for the first time on this watch, its database. The 25 audio modules load, and the kernel log shows nothing new. See [Trial](#trial).
- **Not permanent yet.** Writing it to `boot` waits for a day on the trial, then the user's OK (BUILD.md, "Making a kernel permanent").

## Why

The kernel had `# CONFIG_SYSVIPC is not set`, so every SysV IPC call failed with `ENOSYS`. mkcal, the calendar store that the Agenda app (asteroid-calendar) and Sonic's calendar import both use, locks its database with SysV semaphores (`semget`, `semctl`, `semop` with `SEM_UNDO`). It can't even open the database: "Unable to get semaphore …: Function not implemented (38)". So Agenda has never worked on this watch, and `~/.local/share/system/privileged/Calendar/mkcal/` stayed empty.

## The catch: the stock module ABI

Since the `sound` branch, the watch loads Mobvoi's 25 stock `/vendor/lib/modules/audio_*.ko` at every boot. That works only because this kernel's symbol CRCs equal the stock kernel's, and the stock kernel was built without SysV IPC.

Upstream 4.9 has the SysV IPC members in the middle of `struct task_struct`, right after `nameidata`:

```c
#ifdef CONFIG_SYSVIPC
/* ipc stuff */
	struct sysv_sem sysvsem;	/* 4 bytes on ARM */
	struct sysv_shm sysvshm;	/* 8 bytes */
#endif
```

Turning the option on there breaks the modules twice over. Measured with a control build that was never flashed (build 901: `CONFIG_SYSVIPC=y`, upstream `task_struct`):

- **Offsets.** 78 members move, by 12 or 16 bytes: everything from `fs` (offset 1128 → 1140) on, with `files`, `signal`, `sighand` and `pending` among them, and `thread` (1600 → 1616) as well. The modules were compiled with the old offsets, in inline helpers and `current->…` accesses.
- **CRCs.** genksyms hashes the full definition of every struct a symbol's type reaches, and many reach `task_struct` (through `struct mutex`, for one). 6174 of the kernel's 9211 exports change their CRC, `module_layout` included. Against the stock modules, `modcrc.py` gives 149 match, 344 mismatch: no module would load.

`sysv_sem` and `sysv_shm` are the only structs in `include/` and `arch/arm/include` whose layout depends on `CONFIG_SYSVIPC`, `CONFIG_SYSVIPC_SYSCTL` or `CONFIG_IPC_NS`, and `task_struct` is the only place either is used. (`struct ipc_namespace`, `struct sem_array` and `struct shmid_kernel` are unconditional. `ipc_namespace.h` only switches function declarations.)

## The fix

| Commit | What |
|---|---|
| `e6f6eb3de2a6` | **sched: keep task_struct's layout and CRCs with CONFIG_SYSVIPC.** Moves `sysvsem` and `sysvshm` to the very end of `task_struct`, after `thread`, inside `#ifndef __GENKSYMS__`: RHEL's kABI technique. Every other member keeps its offset, and genksyms sees the struct as it is without SysV IPC, so no CRC changes. Without `CONFIG_SYSVIPC`, `task_struct` is unchanged. |
| `fc9ef7783c50` | **ARM: configs: asteroidos-rubyfish: enable System V IPC.** `CONFIG_SYSVIPC=y`; `CONFIG_SYSVIPC_SYSCTL=y` follows (`kernel.sem`, `kernel.shm*`, `kernel.msg*`). `# CONFIG_IPC_NS is not set` is explicit, see below. The file is still exactly what `make asteroidos-rubyfish_defconfig` writes. |

Why the end of `task_struct` is safe on ARM:

- The comment "It *MUST* be at the end" is about x86 (and s390), whose `thread_struct` ends in a variable-sized FPU state (`CONFIG_ARCH_WANTS_DYNAMIC_TASK_STRUCT`). ARM's `thread_struct` (`address`, `trap_no`, `error_code`, `debug`) has a fixed size. An `#error` stops the build if the option is ever set.
- Nothing on ARM depends on `thread` being last:
  - `asm-offsets.c` only takes `active_mm` and `stack_canary`;
  - `INIT_TASK` uses designated initializers;
  - `arch_task_struct_size` is `sizeof(struct task_struct)`;
  - `thread_info` is on the stack, not in `task_struct` (no `THREAD_INFO_IN_TASK` on 4.9 ARM).
- Only `ipc/`, `kernel/fork.c`, `kernel/exit.c` and `kernel/kcmp.c` (not built: no `CHECKPOINT_RESTORE`) use the two members, always by name. No module allocates or embeds a `task_struct`.
- `sizeof(struct task_struct)` doesn't even change. The struct is 64-byte aligned, and the 12 bytes fit in its tail padding: it is 1792 bytes either way.

`CONFIG_IPC_NS` would also follow, since it defaults to `y` under `NAMESPACES`. It stays off, like `UTS_NS` and `PID_NS`, so the set of namespaces doesn't change: there's no `/proc/<pid>/ns/ipc`, and `unshare(CLONE_NEWIPC)` still fails with `EINVAL`, as before. POSIX message queues stay off, and nothing else in the config changes.

## Checks of build 023

Build 023 is `sysvipc` @ `fc9ef77`, built 2026-11-01 00:51 UTC, against permanent build 022 (`#22-13ebf00`, the kernel source of `asteroidos-2.1`):

| Check | Result |
|---|---|
| `verify.py --allow-config-diff` | all checks pass; ramdisk, DTBs, header and cmdline as in the release |
| config vs 022 | exactly 3 lines: `CONFIG_SYSVIPC=y`, `CONFIG_SYSVIPC_SYSCTL=y`, `# CONFIG_IPC_NS is not set` |
| `modcrc.py` vs the 25 stock modules | 493 match, 0 mismatch, 0 missing; 1482 entries match (395 symbols resolved between the modules). The local copies have the same sha256 as the watch's `/vendor/lib/modules`. |
| `Module.symvers` vs 022 | **byte-identical**: all 9211 exports, the same CRCs, none added or removed |
| vermagic | unchanged, `4.9.232 SMP preempt mod_unload modversions ARMv7 p2v8` (kernel and in-tree modules) |
| `task_struct` layout (pahole on both builds' DWARF) | all 181 members of 022 at the same offset and size. `sysvsem` is at 1740 and `sysvshm` at 1744, right after `thread` (1600 + 140). The size stays 1792, with tail padding 52 → 40. |
| every struct in `vmlinux` (pahole, 9395 types) | none removed. 29 new, all SysV IPC ones (`sem_undo`, `shm_file_data`, `semid64_ds`, …). `task_struct` is the only existing struct that changed. |

The test program, [`sonic/semtest.c`](../sonic/semtest.c), builds to a static ARM binary of about 5 KB. It makes raw system calls and needs no libc, so the kernel's own cross compiler builds it. BUILD.md's trial checks (step 4) give the build and run commands. Run as `ceres`, it:

- does what mkcal does: looks a key up with `semget`, creates the set with `IPC_CREAT|IPC_EXCL`, then runs `SETALL`, `GETVAL`, and lock and unlock with `semop` and `SEM_UNDO`;
- checks that a held lock blocks, with `IPC_NOWAIT` and with a 200 ms `semtimedop`;
- reports how often a system suspend cuts such a wait short with `-EINTR` (see [Trial](#trial), step 5);
- exercises both moved members:
  - a child exits holding the lock, and the kernel must undo it (`exit_sem`, `sysvsem.undo_list`);
  - a child creates, writes and leaves a shared memory segment that the parent then finds by key (`newseg`/`exit_shm`, `sysvshm.shm_clist`).

It removes everything it creates. On build 022 it stops at the first `semget`: `-38` (`ENOSYS`).

## Trial

The 2026-11-01 trial followed BUILD.md, off the cable.

1. **Flashing.** `flash-recovery.sh` passed every check, with `boot` holding the permanent build 022 image, then wrote `023-sysvipc/boot-trial.img` to `recovery` (sha256 `69215176…`, read back identical). The reboot went out at 01:03:26 UTC (`systemctl reboot --reboot-argument=recovery`).
2. **Network.** SSH was back over Wi-Fi 36 s later, at 16.7 s of uptime. `kernel-trial-wifi.service` ran and found connman ready at its first look (30 s), so it exited without doing anything.
3. **Health.** `trial-check.sh` reported healthy at 22 s and again at 164 s: trial cmdline, `uname -v` and `/proc/config.gz` of this build, download mode off, Wi-Fi, Bluetooth, ioslink, display, touch and launcher.
4. **SysV IPC is on.**
   - `/proc/config.gz` has `CONFIG_SYSVIPC=y` and `CONFIG_SYSVIPC_SYSCTL=y`; `IPC_NS` and `POSIX_MQUEUE` are off.
   - `/proc/sysvipc/` has `msg`, `sem` and `shm`.
   - `kernel.sem` is `32000 1024000000 500 32000`, the 4.9 defaults.
5. **`sonic-semtest` as `ceres`:** every step passes, including the kernel's undo of a dead child's lock and the orphaned segment found by key. Each run removed everything it created, and the binary was deleted from `/tmp` after each test.
   - **Waits end in `-EINTR` at every suspend attempt.** A second run, at 625 s, failed one step: its 200 ms `semtimedop` on the held lock returned `-EINTR` instead of `-EAGAIN`. Every suspend attempt freezes user tasks with a fake signal, and `semop`/`semtimedop` are never restarted after a signal (signal(7), and `ipc/sem.c` here). The attempt counts even when the suspend is then aborted.
   - Off the cable, this watch tries about twice a second: 30 attempts between 01:16:24 and 01:16:40 UTC, 21 of them aborted by a pending wakeup.
   - This is upstream behaviour, unrelated to the moved members. mkcal (`dd6e56a0`, the AsteroidOS recipe's revision) already retries: `semaphoreIncrement()` in `src/semaphore_p.cpp` loops `while (errno == EINTR)`.
   - The test now retries too, and reports the count. In three more runs, at 761 to 770 s, it passed with 0, 9 and 10 interrupted 200 ms waits. A 3 s wait, not retried, ended after 0.8, 0.8 and 2.8 s; during the first, the watch was suspended for 0.6 s.
6. **mkcal works.** asteroid-launcher links libmkcal. At boot it created mkcal's semaphore set (`/proc/sysvipc/sem`: 3 semaphores, mode 0777, owner `ceres`) and its database, `~/.local/share/system/privileged/Calendar/mkcal/db`. Before, that directory had always been empty.
7. **Audio.** `sonic-audio.service` is active (success) with all 25 `audio_*` modules loaded, no module load errors (no "disagrees about version", no unknown symbols) and the sound card present. Nothing was played.
8. **Kernel log.**
   - The only oops/BUG/WARN lines are the two known boot WARNINGs (`sysfs_warn_dup`, `pm_qos_add_request`).
   - There are no touch or I2C errors.
   - Over the first 90 s, `klog-compare.py` against a full normal boot of build 022 shows only known differences: version strings, a Wi-Fi deauthentication, the touch controller's mini-init and the nanohub LCD lines at screen changes, and suspend timing.
   - Off the cable, the watch suspends as usual: 43 suspends by 140 s.
9. **Bluetooth.** The iPhone was connected again by 61 s: one connected device. ioslink logged `ancs: subscribed` and the Sonic app's `bridge: hello` at 64.6 s.
10. **Confirmed** at 164 s (01:06:36 UTC) with `trial-check.sh --confirm`. The dead-man's switch left it running past 600 s.

## What remains

- A day on the trial kernel, with the user's usual use, then the user's OK to make it permanent, as for build 022: build `boot-final.img` from `023-sysvipc/boot.img`, write `boot`, reboot, check, put the 2.1 release back in `recovery` (`flash-recovery.sh --fallback`), and add the image to `sonic/known-good-boot.sha256`.
- Then Sonic's calendar import and the Agenda app can use mkcal. That is userspace work, outside this branch. `ceres` lingers (`loginctl`: `Linger=yes`), so logind's default `RemoveIPC=yes` never removes mkcal's semaphores while the watch runs.
- Adding more members to `task_struct` (or to any struct the stock modules see) later: append them at the end and hide them from genksyms the same way, then check with `modcrc.py`, a `Module.symvers` comparison and pahole.
