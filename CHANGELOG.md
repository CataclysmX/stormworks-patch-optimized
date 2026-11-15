# Changelog

stwpatch ("Cata-ptimised"): performance and stability patch for Stormworks (stormworks64.exe,
server64.exe), loaded as a proxy `OpenAL64.dll` (client) or `opengl32.dll` (dedicated server).
The version is shown in the main-menu label (`v1.15.x 64Bit Cata-ptimised v1.2 (date)`) and at the
top of `stwpatch.log`.

## [1.2.2] - 2026-11-15

### Fixed
- **Game freeze on a stale interface object** (game bug, also without the patch:
  `crash_15_11_cata`, frozen after ~4 h). An interface list of 16 numbered lines
  ("N: name" / "N: empty", refreshed ~60 times per second on the main thread) pointed to an
  object that had already been freed. Its name length read 3.86 billion, so the game scanned
  ~3.9 GB of memory, then its buffer-growing function looped forever: it doubles a 32-bit
  capacity, which overflows to 0 above 2^31.
  - Lines whose object is unreadable, or whose name is absurdly long, are now shown as empty
    (the game's own branch).
  - A buffer request above 2^31 (which the game could never satisfy) now writes a clear line
    in `stwpatch.log` and stops the game with an exception, so the crash reporter writes its
    dump at once instead of leaving a frozen game.
  - `[fixes] hang_guard=1`. Tested by `test/hang_guard_test.c` on byte-identical copies of both
    game sites.

### Build
- mimalloc is now a git submodule (`third_party/mimalloc`, v2.1.7). It was missing from the
  repository, so a clone could not be built. Clone with `--recursive`, or run
  `git submodule update --init`.

## [1.2.1] - 2026-11-15

### Fixed
- OpenGL filter: each time the game looked up a GL function again, the patch made a new
  128-byte thunk for it and never freed it. One thunk per function now. This is far too small
  to explain a whole machine running out of memory (that case is still being investigated, see
  below), but it was unbounded.

### Added
- **Memory line in `stwpatch.log` every 60 s**, to find out where memory goes when it grows:
  - private memory and its growth since the last line and since the first one;
  - working set, mimalloc commit (peak) and number of thread heaps;
  - system RAM free / total and commit / limit;
  - threads, handles, GDI objects, GL thunks.

## [1.2] - 2026-11-14

### Fixed
- **Crash caused by the patch while profiling** (only with `[profile] sampler_ms` set, during a
  world or vehicle load). The sampler unwound the stack of a game thread that was exiting; that
  stack was already freed, so the read faulted. While logging that fault, the crash logger read
  past the top of the stack, and that second fault crashed the game. Fixed:
  - the crash logger never reads past the committed stack region;
  - the sampler unwinds under a guard: an unreadable stack only drops that sample;
  - the sampler skips threads that have exited.

### Changed
- **"Unstick" (wheels / blocks stuck under the map) is now off by default.** In game it fired on
  parts of a heavy vehicle at sea level next to an island that were not stuck. Their contacts
  with the ground were dropped and they could pass through it. The setting is renamed
  `[fixes] unstick_enabled` so that existing installs, whose merged ini keeps the old
  `unstick=1`, get it off too. It can still be turned on live in the F4 menu.
- **Sampler** (`[profile] sampler_ms`, diagnostic, off by default):
  - stacks are unwound through every loaded module (up to 48 frames), so waits inside Windows /
    Wine functions are attributed to the right game function (the old stack scan picked stale
    return addresses);
  - the report adds inclusive time per function, the hottest instructions and a call tree.
- Version 1.2.

### Upgrade notes
- Turn `sampler_ms` back to `0` after a measurement: it suspends every watched thread every
  N ms and slows the game down.

## [1.1] - 2026-11-14

### Fixed
- **Crash in game after a long session**. The game reads memory it has already freed, a latent
  bug that the Windows heap hides. The bundled allocator (mimalloc) returned freed pages to the
  system after 10 ms, so these reads crashed. It now keeps them (`[memory] allocator_purge=0`).
  This fixes the crashes reported at `stormworks64.exe+0x7f9b73` and `+0x58b280`.
- **Crash in the vehicle editor when deleting parts with symmetry on** (game bug, also without
  the patch). The editor read the mirrored part after deleting it. The patch now reads it
  before the delete (`[fixes] editor_mirror_delete=1`).

### Added
- **F3 debug overlay**, a transparent panel over the game showing:
  - simulation ticks/s and the share of slow ticks (slow motion), physics step, rate;
  - bodies, contacts, waits, and the slowest collision pair (which two objects);
  - memory, network, active fixes, last exception.
  
  It works with OpenGL and Vulkan, under Proton and Windows (not over exclusive fullscreen on
  Windows).
- **F4 settings menu**: changes most settings live, shows each setting's default value and saves
  to `stwpatch.ini`. Settings that only apply at the next launch are marked `*`.
- **Compound split**: the collision of one big vehicle (thousands of physics shapes) against the
  terrain is now spread over the collision threads. Measured on a 4 100-shape vehicle: collision
  pass 7.7 ms → ~3 ms, other threads no longer waiting for it (82 % → 0 %).
  `[physics] compound_split=1`.
- **OpenGL unbind filter**: useless `glBindBuffer(target, 0)` calls followed by another bind are
  not sent to the driver (~30 000 calls per frame removed on a heavy save, identical image in
  the test). `[misc] gl_filter=1`, live in F4.
- **Frame limiter** `[misc] fps_cap` (live in F4).
- **Workbench wireframe option** `[editor] wireframe`: skips the editor's second wireframe
  pass over the vehicle (live in F4).
- **OpenGL statistics** (`[profile] gl_stats`): draws, binds and redundant binds per frame, in F3
  and in the log.
- **Unstick** (`[fixes] unstick`): drops the ground contacts of a small part pushed down into
  the terrain for 1 s (wheel stuck under the map). Turned off by default in 1.2 (false
  positives).
- Patch version in the main-menu label and in the log (version, build date, commit).
- The crash log records the state of the faulting memory (committed / decommitted / released,
  inside the allocator), every register and the code bytes around the fault.

### Changed
- `install.sh` and `install.bat` **merge new options into an existing `stwpatch.ini`**: the
  user's values are kept and a `.bak` copy is written. Reinstalling keeps `OpenAL64_orig.dll`.

## [1.0] - 2026-10-25

First release.

### Added
- **Parallel vehicle tick**: the per-vehicle simulation tasks, which the game runs one after
  the other on a single thread, run on a pool of threads (`[vehicle] threads`).
- **Parallel collision detection** (Bullet narrowphase) over several threads
  (`[physics] narrowphase_threads`). Pairs with soft bodies (ropes) stay serial.
- **Physics rate governor** (`[physics] rate=-1`, `budget_ms`): runs physics at the highest
  rate among 60 / 120 / 180 Hz that fits in the budget, based on how long the simulation really
  waits for physics. `rate=60|120|180` forces a rate, `rate=0` keeps the game's choice.
  - Measured on a save with 168 vehicles: 21.7 → 60 ticks/s, physics step 37.8 → 7-8.5 ms.
- **Faster memory allocator** (mimalloc) for the game's heap (`[memory] allocator`).
- **Multiplayer network settings** (`[network]`): larger Steam send buffer and send rate per
  player (Steam's defaults, 512 KB and 256 KB/s, make players "slow" / "desynced" when the host
  runs at 60 ticks/s), plus a per-player report (`stwpatch_net.txt`).
- **Dedicated server support** (server64.exe through an `opengl32.dll` proxy).
  `steam_api64.dll` is never modified.
- **Crash guard**: crashes are logged with module and offset. After a crash during startup, the
  next launch starts in safe mode, without patches.
- **Diagnostics**:
  - physics report (`stwpatch_physics.txt`: Bullet phases, bodies, waits);
  - allocation tracker for memory leaks (`[memory] track`);
  - sampling profiler (`[profile] sampler_ms`).
- Version suffix in the main-menu label (`[misc] version_suffix`). The game's version string
  used by multiplayer checks is unchanged.
- Installers: `install.sh` (Linux / Proton), `install.bat` / `uninstall.bat` (Windows).

### Known limitations
- With parallel collision detection, the order of contacts is no longer deterministic. It is
  physically valid, but two runs of the same scene diverge.
- Still single-threaded in the game: Bullet's constraint solver, most of the vehicle logic,
  Lua scripts, rendering.

[1.2.2]: https://github.com/CataclysmX/stormworks-patch-optimized/compare/04f1da3...HEAD
[1.2.1]: https://github.com/CataclysmX/stormworks-patch-optimized/compare/cd2b9b8...04f1da3
[1.2]: https://github.com/CataclysmX/stormworks-patch-optimized/compare/0b6b308...cd2b9b8
[1.1]: https://github.com/CataclysmX/stormworks-patch-optimized/compare/Realese...0b6b308
[1.0]: https://github.com/CataclysmX/stormworks-patch-optimized/releases/tag/Realese
