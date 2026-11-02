# Big-endian progress: where to pick up

State on 2026-11-02 (branch `claude/eloquent-rubin-gru53s`, aletheia pin
`f47f71260`).

## Reached

- Big-endian (qemu-ppc) matches little-endian pixel for pixel through
  frame 10000 (boot, intro, naming, the professor's speech) and within ~1%
  through the intro fly-over (frames 10500-11000).
- Little-endian plays to frame ~42000: both rival battles, the house, the
  Xtransceiver, Nuvema Town, Cheren in front of the lab ("go to Bianca's
  house").

## Next blocker (big-endian, frame ~11269)

Right after the fly-over the new game's map change runs its init script,
then the game's heap allocation fails:

    bw-port: PANIC in unimplemented game function at 0x02030B50: heap report:
    reached only after the host allocator failed

Most likely a size or count read from field data that is still in the DS's
order (zone, matrix, map, NPC or script files).  Get the stack with
`--watchdog`-style tracing or a breakpoint on the allocation failure, find
the size's source, and turn that format at extraction
(`install/byteorder.c`) or store its struct in the DS's order
(`tools/sso_types.txt`), as for the zone event files and zone headers.

The freeze before it (the init script spinning in SCRIPT_SYS_ForceExitNet)
is fixed: ov010's permit table needed both the byte typing
(`tools/armdata_widths.txt`) and its bit order (`tools/sso_types.txt`).

## How to run

    cd ../bw-work/build-be32 && cmake . && make -j4 eidos_boot eidos_extract
    cd .. && rm -rf extract-be && cp -r extract extract-be
    QEMU_LD_PREFIX=/usr/powerpc-linux-gnu qemu-ppc build-be32/eidos_extract --to-host-order extract-be
    cd build-be32
    EIDOS_RTC="2026-06-01 12:00:00" QEMU_LD_PREFIX=/usr/powerpc-linux-gnu \
        ./eidos_boot --watchdog 45 --frames 13001 --input ../play/c.txt \
        --screenshot 11000,11500,12000 --screenshot-dir ../play/be11

- Fix the clock (`EIDOS_RTC`) for every comparison run: the RNG and the
  time-of-day lighting follow it.  The little-endian reference with that
  clock is `play/le5` (frames 5000-40000, every 500).
- `--watchdog N` prints the stuck thread's stack (PowerPC back chain);
  `powerpc-linux-gnu-addr2line -f -e eidos_boot ADDR...` names it.
- `cmake .` is needed after editing `tools/armdata_widths.txt`; the data
  mark (`sdk/types.h`) forces regenerating `extract-be`.
- Input scripts are in `bw-work/play` (`c.txt` to frame 40000; `p.txt`
  reaches Cheren at the lab on little-endian).
