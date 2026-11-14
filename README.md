# stwpatch: Stormworks (stormworks64.exe) optimization

Analyzed build: `stormworks64.exe` from 2026-09-11, sha256 `f9206d85…8699`
(PE timestamp `0x6a9a9e20`). The code patches only activate on this exact build. The
memory tracker works on any version.

## Reverse-engineering findings (Ghidra)

### Threads: the game is not single-threaded, but almost
`mm_thread_object` (`0x14017d3c0`) creates one worker thread per instance, each with a FIFO queue.
Instances found (name passed to the constructor):

| thread | created by | role |
|---|---|---|
| main | n/a | game logic, Lua, rendering, waits for the others |
| `physics` | `c_scene` ctor `0x1405e3b50` | Bullet `btDiscreteDynamicsWorld::stepSimulation` (`0x1405e3a80`) |
| `vehicle_tick` | `c_vehicle_local_container` `0x140546c60` | simulation of **every** vehicle, one after another |
| `vehicle_save`, `streaming`, `http`, `state_transition` | n/a | I/O, loading |

* **Vehicles (the bottleneck addressed here)**: `FUN_1404bab00` loops over every vehicle body and
  allocates a `threaded_tick_task` (0x110 bytes + mutex + condvar) for each one **every tick**, then queues it
  onto the single `vehicle_tick` thread. The tick (`FUN_14048ad30`, ~2900 decompiled lines, 200
  functions in its call tree) only writes the body's own data. The only global writes in the
  tree are thread-safe C++ magic statics and CRT code. The shared grids
  (`FUN_140310750`) are mutex-protected. The ticks of different bodies can therefore run in parallel.
* **Physics**: one Bullet world, `btSequentialImpulseConstraintSolver` (single-threaded),
  called with `maxSubSteps = 1000`. The main thread submits the step (`0x1407439df`) and, when the flag
  `[state+0xa68]` is 0, **waits for it immediately** (`0x140743a01`), so physics and logic don't overlap.
  Replacing Bullet with its multithreaded version is not feasible from outside the binary (data
  structures compiled in, no sources). **Not patched.**
* Several loading waits poll with `Sleep(1)` (`0x1402e4d34`, `0x14073f37d`, `0x140774a75`…).

### Memory
* No obvious leak can be proven statically. The candidates require runtime measurement.
  Allocation churn is high: one `new` + mutex + condvar per vehicle per tick (60 Hz),
  which fragments the heap (all the more under Wine).
* The CRT is statically linked: every `malloc/new` goes through the exe's import of `HeapAlloc`, which makes it
  possible to observe every allocation from outside.

## Physics (Bullet): detailed analysis

Embedded Bullet ≈ 2.88, **double precision**, `btSoftRigidDynamicsWorld` world (ropes = soft bodies),
`btDbvtBroadphase`, `btSequentialImpulseConstraintSolver`, **profiler compiled in** (`BT_PROFILE`).
Physics commands from the main thread go through an event queue (`mm_physics_scene::event_*`)
executed on the `physics` thread.

| point | address | finding |
|---|---|---|
| tick loop | `FUN_140865be0` | **1 logic tick per frame**, no catch-up: when it's overloaded the simulation slows down (slow motion) and the FPS drops with it. No spiral of death. |
| physics step | `0x1407439df` | always 1 tick (`[task+0xd0]=1`), `stepSimulation(1/60, maxSubSteps=1000, 1/60)` |
| sync | `0x140743a01` | if `[state+0xa68]==0` (toggled by an internal event, type 0x39), the main thread waits for physics **immediately** → no overlap between physics and logic |
| solver | world`+0xa8` | `btContactSolverInfo` left at defaults: **10 iterations always**, residual threshold = 0 (never stops early, `FUN_14012e370`) |
| sleeping | `FUN_1401348a0` | supported. Game thresholds: linear 1.6 / angular 2.5 (more aggressive than Bullet), `gDeactivationTime` = 2 s |
| AABB | world`+0x88=0` | only active objects get their AABB updated (good) |
| contacts | `gContactAddedCallback = FUN_1401a35c0` | pure function (triangle normal correction) |

### Improvement areas (from most to least promising)
1. **Measure first**: `[physics] profile=1` writes `stwpatch_physics.txt` (Bullet phases, bodies,
   manifolds, constraints, main thread waits). That decides between areas 2-4.
2. **Solver early exit** (`solver_residual`, implemented): stops the 10 iterations once
   contacts have converged. Free on calm scenes. Watch for slightly softer contacts.
   `solver_iterations` (6-8) as a last resort.
3. **Parallel narrowphase**: **implemented** (`patch/src/narrowphase.c`, see below). Original analysis:
   the pairs are independent. The collision algorithm has no shared state (local GJK
   solver, stateless EPA). The only shared structures are the dispatcher's pools (`findAlgorithm`,
   `getNewManifold`, `releaseManifold`, `allocate/freeCollisionAlgorithm` = vtable slots 1/2/3/13/14),
   which a lock can protect. Pairs involving a soft body (ropes) must stay serial.
   Expected gain: proportional to the share of `dispatchAllCollisionPairs` in the profile (often the
   largest with compound vehicles). Risk: different but valid contact order (non-deterministic).
4. **Physics / logic overlap**: force the deferred wait (`0xa68` flag) → physics runs during the
   rest of the tick. Semantics of the flag not established (dev toggle?), risk of
   rendering/logic reading transforms mid-write. Experimental.
5. **Parallel solver per island**: Bullet ≥2.87 does this (`btSimulationIslandManagerMt`), but
   it isn't compiled into the game. Reimplementing it from outside = very risky.
6. Not feasible from outside: replacing Bullet, changing the vehicle collision shapes (compounds
   of blocks), decoupling rendering from ticks.

## What the patch does (`patch/`, proxy `OpenAL64.dll`)

The DLL replaces `OpenAL64.dll` (the original is renamed `OpenAL64_orig.dll`, all 167 exports
are forwarded to it). It loads before the game's CRT, with no Wine override needed.

1. **Parallel vehicle tick** (`[vehicle] threads`): hook on `mm_thread_object::enqueue`
   (`0x14017d720`). The `threaded_tick_task` tasks (identified by vtable) go to a pool of N threads
   (`stw-vehicle-N`), and every other task stays on the original thread. Completion follows the game's
   protocol (`FUN_14017d800`), with an additional fix: the "pending" flag is cleared under the
   mutex (the original code has a small use-after-free window).
2. **Allocation tracker** (`[memory] track=1`, diagnostic): samples 1/N allocations
   (all allocations ≥ 256 KiB) and records 2 return addresses in game code. Writes
   `stwpatch_heap.txt` every minute: **the call sites whose memory keeps growing = leak
   candidates**, with their RVA (directly decompilable in Ghidra: `0x140000000 + rva`).
3. `timeBeginPeriod(1)` (precision of the `Sleep(1)` polling loops).
4. **Physics profiler + solver** (`[physics]`): hook `stepSimulation` (vtable slot 12 of both
   world classes). After each step it reads the game's Bullet profiler tree
   (per-thread roots `0x140cbf730`, nodes `name@0 calls@8 time_ms@0xc child@0x20 sibling@0x28`),
   times the main thread's waits on physics (`mm_thread_task::wait` `0x14017d2c0`) and on vehicles
   (destructor of `threaded_tick_task`), and applies `solver_iterations` / `solver_residual`.

Safety checks: PE build verification + byte signatures before any patch. Otherwise the patch disables itself
and says so in `stwpatch.log`. The IAT hook is all-or-nothing.

5. **Parallel narrowphase** (`[physics] narrowphase_threads`, on by default): replaces
   `dispatchAllCollisionPairs` (dispatcher vtable slot 7). The pairs are spread across N threads
   `stw-collide-N` + the physics thread, in chunks of 8. Slots 2/3/13/14 (manifolds and algorithm
   pool) go through a lock. Per-triangle temporary algorithms use a per-thread cache
   that gets flushed at the end of the pass. Bullet profiler indices are assigned under a lock.
   Pairs involving a soft body (ropes) run serially afterwards. Fallback to the
   original if: < `narrowphase_min_pairs` pairs, non-default near callback, non-discrete (TOI) mode,
   or a different pair cache.
   Stress test `patch/test/narrow_test.c` (fake dispatcher with the game's layout and
   non-thread-safe pools): 2000 passes × 3000 pairs, 0 errors, ×6.2 on the synthetic benchmark.
   Contact order is no longer deterministic (physically valid, but two runs diverge).

6. **Physics rate** (`[physics] rate`): the game switches Bullet between 180/120/60 Hz
   (3/2/1 sub-steps per tick, `0x140743916`): wait > 1 ms → it lowers the rate, < 0.5 ms → it raises it,
   every 900 ticks, but only with samples from its deferred-wait path. Under load it stays at 180 Hz.
   `rate=60|120|180` forces a rate, `rate=-1` = stwpatch governor (highest rate whose
   step time fits in `budget_ms`).

### Measurements (save with 168 vehicles, ~8,500 contact manifolds)
| | ticks/s | physics step | main thread waits on physics |
|---|---|---|---|
| original (vehicle pool only) | 21.7 | 37.8 ms | 33.6 ms |
| + parallel narrowphase | ~37 | ~17.5 ms | ~13 ms |

With the narrowphase in parallel, collision detection is still 63% of the step (10 ms), and the solver 3 ms (iterations only 0.5 ms).

### Measurements with `rate=-1` (same save)
| | ticks/s | physics step |
|---|---|---|
| original | 21.7 | 37.8 ms |
| vehicle pool + parallel narrowphase | ~37 | ~17.5 ms |
| + governor (`rate=-1`, drops to 60 Hz) | **60** | **7-8.5 ms** |

## Portability: client, server, future builds
`patch/src/resolver.c` finds every address at run time (RTTI names for the vtables, byte patterns
for `enqueue`/`wait`, the calls they contain, the profiler through `lea 'internalSingleStepSimulation'`).
`tools/resolve.py` does the same offline and checks it against the Ghidra values (client: 22/22).
`server64.exe` (PE ts 0x6a9a9e9e): same game code for the threads (identical bytes), Bullet compiled
differently (profiler through `bts_enterFunc/leaveFunc` pointers, thread index at TLS+4 instead of +8,
no per-step profiler reset → approximate phases), same structure offsets (checked one by one).
The server doesn't import OpenAL64 → the `steam_api64.dll` proxy (same code). Output files are prefixed `stwpatch_server`.

## Robustness
* `crashguard.c`: an exception handler logs code, module+offset and the stack's return addresses
  into the log; `stwpatch.running` marks the startup stage. After a crash during startup, the next launch
  runs in **safe mode** (no patches) and says so (`[misc] safe_mode=1`). For an access violation it
  also logs whether the faulting address is committed / decommitted / released and inside the
  mimalloc heap, every general register, and the 32 code bytes around `rip`.
* Allocator: crash of 2026-11-08 (build of 2026-10-29), main thread at `stormworks64.exe+0x7f9b73`,
  read of `0x29a86510e18`: an address in mimalloc's 2-6 TiB range, i.e. the game read a heap block it
  had already freed. mimalloc decommits freed pages after 10 ms (the Windows heap keeps them), so the
  game's latent use-after-free became a crash. mimalloc now never purges (`[memory] allocator_purge=0`);
  `allocator=system` turns mimalloc off entirely.
* The use-after-free itself (`editor_fix.c`, `[fixes] editor_mirror_delete=1`): `exe+0x7f9b73` is in
  `c_game_state_vehicle_editor` vtable slot 12 (FUN_1407f6c10). Deleting parts with symmetry on, it
  deletes the mirrored part (FUN_1404c2a80 → FUN_1404ac3d0 → the part's deleting destructor), then
  reads `part+0x58` (its definition) from the freed object for FUN_14080c1c0. Two sites (selection:
  `r14`, single part: `rdi`), found by byte pattern; the patch loads the definition into that
  callee-saved register before the delete and passes it after: same calls, same order.
* Crashes at `exe+0x58b280` (2026-11-14, another player, DLL of 2026-10-25): FUN_14058b200, called
  every frame from FUN_1407425a0 → FUN_1405ebd20, walks a list at world+0x5f28 whose entries hold
  weak handles (dead flag +0, object +8, type +0x10) and interpolate the object's transform. A
  handle was read after being freed, while many vehicles despawned; stale return addresses on the
  stack (`fa_free`, `_mi_os_purge_ex`) show the thread had just freed memory and mimalloc had
  decommitted it. Same class as above: covered by `allocator_purge=0` (the Windows heap keeps such
  pages too). Who frees the handle too early is not identified (virtual calls; REA's Ghidra runs
  out of memory at 2 GB on this exe).
* `unstick.c` (`[fixes] unstick=1`): game bug, with or without the patch: a wheel or block caught
  under the terrain is pushed down by it while its vehicle pulls it up (jitter, many contacts).
  After each narrowphase, a dynamic body of radius ≤ `unstick_max_radius` (3 m: never a whole
  vehicle) pushed DOWN (contact normal against the gravity read from world+0x1e0) into static
  geometry deeper than `unstick_depth` (0.3 m) for `unstick_seconds` (1 s) has its contacts with
  static geometry dropped (manifold point count 0) until static geometry pushes it up again for
  0.2 s, or 2 s without contact. Layout notes in `unstick.c`; logic tested natively by
  `test/unstick_test.c` (`gcc -Itest/linux_shim test/unstick_test.c`). The physics report shows
  the counts.
* Windows: `dist/windows/` (`install.bat` / `uninstall.bat`, guards on DLL sizes) and `dist/stwpatch-windows.zip`.

## Multiplayer (in-game hosting): "slow" / "desynced" players
* The player list classifies a per-player "ticks behind" counter: < 5 synced, < 10 slow, < 50
  desynced (`FUN_140793e20`).
* All game traffic goes through `ISteamNetworkingMessages002` (`SendMessageToUser`...). Its sessions use
  Steam's **global** settings, which the game never changes. Values read from Steam: send buffer
  512 KB, send rate **min = max = 256 KB/s per player**. Beyond that, the queue grows (player behind) then
  Steam refuses sends (`k_EResultLimitExceeded`, message lost). A host at 60 ticks/s (thanks to the patch)
  produces ~3x more updates than at 22 ticks/s.
* `net.c`: IAT hook of `SteamInternal_FindOrCreateUserInterface` / `...GameServerInterface` in the
  exe, a forwarding wrapper of the interface (6 methods, checked against Valve's headers) that
  counts per player, a `stwpatch_net.txt` report (ping, quality, real rate, pending bytes, queue time,
  refused sends) and `[network]` config via `SetGlobalConfigValueInt32` (public Steam API).
  No Steam file or Steam memory is modified.

## Debug overlay (F3)

`overlay.c`, `[misc] overlay_key=F3`: F3 while the game has the focus toggles a semi-transparent
panel over the top-left of the game, refreshed every 250 ms (figures over the last second):
patch version / build date / commit, simulation ticks/s and % slow ticks (slow motion), physics
steps/s, step time, rate (governor), objects / dynamic bodies / contact manifolds / constraints,
main-thread waits, narrowphase (threads, pairs, pass time, % waiting for the last worker) and the
**slowest collision pair** (≥ 1 ms: both bodies' shape type, bounding radius, static or not,
position), vehicle ticks/s, unstick counts, memory (private, working set, mimalloc commit),
network (players, KB/s, refused sends), active fixes, last exception. It is a separate topmost
click-through window that never takes the focus (GDI, no renderer hook): it works with Vulkan or
OpenGL, under Proton or Windows, but not over an exclusive fullscreen mode on Windows.
The slowest pair is also written to the physics report and logged above 2 ms (at most every
30 s): on 2026-11-14 a single pair took 6.9 ms of a 6.9 ms pass (85 % of it waiting for that
one pair).

## Compound split: one big vehicle on the terrain (`compound.c`)

The parallel narrowphase splits pairs between threads, but one pair stays on one thread: a 36.6 m
vehicle (btCompoundShape, thousands of children) on a 370 m terrain mesh cost 9.6-14 ms per step
while the other threads waited. `btCompoundCollisionAlgorithm::processCollision` (vtable[1]) walks
the compound's AABB tree and calls `btCompoundLeafCallback::ProcessChildShape` (FUN_1400e2960,
found as "called by processCollision and by its tree walk, with the expected prologue", every
.pdata fragment scanned) per overlapping child. While processCollision runs, the hooked
ProcessChildShape only records the children; afterwards they are processed by this thread and by
idle narrowphase threads (`run_chunks` helps while pairs are still running), each with a private
copy of the callback (0x38) and of the btManifoldResult (0x38) whose wrappers ProcessChildShape
swaps. Inline (original order) below 48 children, with a shared manifold, a foreign result class,
or inside a nested compound. `[physics] compound_split=1`; counts in the physics report.
`test/compound_test.c` (wine): every child once, private result copies, caller's result intact,
inline cases; ~69 % of 5000 children on 3 helpers.

## In-game "Performance Warning"

Fixed thresholds in the game (FUN_1405e2xxx, 2026-10-29 build), checked when a vehicle loads, not
measurements: more than 50 physics bodies, 2000 physics shapes, 50 lights or 30 monitors, file
larger than 10 / 14 MB. stwpatch does not change the counts, so the message still appears; what
changes is the cost: shapes and bodies → collision detection (parallel narrowphase), vehicle
logic (vehicle pool), physics rate (governor). Still single-threaded: Bullet's constraint solver,
and everything about lights and monitors (rendering).

## Version label
`branding.c`: the main menu label ("v1.15.23 64Bit", built by `FUN_140261f80`) becomes
"v1.15.23 64Bit Cata-ptimised" (`[misc] version_suffix`). Only that function's suffix changes (2 length
immediates + the `lea` of " 64Bit" redirected to a text placed in the tail of `.rdata`). The `"v1.15.23"` literal
and its 11 other uses (multiplayer compatibility checks, saves, exports) are untouched.

## Usage

```bash
./install.sh install      # client: OpenAL64.dll proxy (+ stwpatch.ini)
./install.sh install server   # + dedicated server: steam_api64.dll proxy
./install.sh status       # shows stwpatch.log
./install.sh uninstall    # restores the original
python3 tools/stwmon.py 5 run.csv   # CPU per thread + RSS while you play
tools/profile.sh 20                 # hotspots by function (requires perf)
```

Recommended test: solo, a save with many vehicles. Compare `threads=0` with `threads=6`
in `stwpatch.ini` using `stwmon.py`: before, `vehicle_tick` sits near 100%. After, the load
is spread across `stw-vehicle-*`.

Physics: play with many vehicles, then read `stwpatch_physics.txt`. Rerun with
`solver_residual=0.00001` and compare `step time` and the `solveGroup*` phases.

Leak hunt: `track=1`, play normally for 30-60 min, then send `stwpatch_heap.txt`
("top growth" section).

## Limitations / risks
* The pool has not been validated in real gameplay. Static thread-safety analysis does not
  cover writes through pointers into other objects (e.g. multi-body vehicles). If you see
  crashes or strange behaviour, set `threads=0`.
* Multiplayer: test in solo first. The game has an anticheat component (`c_anticheat`, Steam
  ticket). Modifying the client could be detected.
* A game update disables the code patches (the tracker keeps working). Steam "verify files"
  restores the original DLL.
* `-force-vulkan` in the Steam launch options is a Unity option, so Stormworks (OpenGL) ignores it.

## Tree
* `ghidra/`: Ghidra headless project (analysis done with 28 GB of heap), `q.sh` + `scripts/Query.java`
  (decompile/xrefs/rtti/global writes), outputs in `ghidra/out/`
* `patch/src/`: DLL sources. `make -C patch` (mingw-w64)
* `tools/`: monitor, profiler, `functions.tsv` (22770 functions exported from Ghidra)
