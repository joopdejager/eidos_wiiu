A synthetic aletheia tree for tests/test_ovlink.c and cmake/overlays.cmake:
the layout of four overlays (0 and 1 share the window 0x02200000, 2 sits at
0x02300000, 230 carries five static initialisers at the addresses the real
ov230's are recorded at), their manifest shards, matched-style sources and
listing annotations, and one ARM9 caller.  Written by hand; no game data.

layout/data_objects.json types each window's table (0x02200100, one u32), so
a big-endian host turns it into its own order when the overlay loads, as
aletheia's own data_objects.json types the real overlays' data.
