# RAM helper experiment

Run the **Patch IPA** workflow with `ram_probe` ticked. It builds `<name>-ramprobe.ipa`: the normal
memory-limit patch plus a `RamHelper.appex` app extension and a probe in `libmemlimit.dylib`.

About 12 seconds after launch the probe starts the helper, borrows 4 x 256 MiB from it (env
`RPCS3_RAMPROBE_CHUNKS` changes the count), touches the pages and writes the results to
`Documents/memlimit.log` (Files app > On My iPhone > RPCS3), lines starting with `[ramprobe]`.

It answers three questions on a real device: does the helper start, how much memory may it lend, and
does lent memory stop counting against the app's own limit. It does **not** make the emulator use the
memory; that needs support inside the core.

`extension.mm` and `protocol.h` are derived from Vita3KiOS (GPL-2.0-or-later), which credits the
"RAM donor" technique to MeloNX (MIT). Keep the licence headers.


## Soft memory budget (ram-helper build)

The core trims its own texture/surface caches when `os_proc_available_memory()` reports little headroom
(levels at 1.5 GiB, 1 GiB and 512 MiB). With the kernel limit raised that never happens, so the build
reports `min(real, budget - footprint - lent memory in use)` to the core only. Budget: 3072 MiB by default;
put a number of MiB in `Documents/ramhelper.budget` to change it, `0` to turn it off. Log lines:
`pressure level the core sees: a -> b` and a `budget:` line every 30 s.
