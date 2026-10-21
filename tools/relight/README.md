# Map relighting (bounce light for compiled maps)

`gi_bounce.py` adds path-traced bounce light to a compiled JKA map (`.bsp`, RBSP) while keeping
its original lighting: compiled maps no longer contain their light entities, so a full rebake
would lose the designer's lights. The existing lightmaps are treated as direct light; each
lightmap texel gathers light from the surfaces around it (their lightmap x texture colour),
repeated for several bounces. The light grid (used to light characters) gets the same bounce.

    pip install embreex numpy pillow scipy
    python3 gi_bounce.py maps/duel_dotf.bsp out.bsp --assets <extracted pk3 dir> \
        --bounces 3 --rays 64 --strength 1.5 [--preview compare.png]

Pack `out.bsp` as `maps/<name>.bsp` in a pk3 that loads after the map's own pk3.

`make_srf.py` recreates the `.srf` file q3map2 needs to run `-light` on a compiled map (a full
q3map2 rebake; loses the original light entities, so not used by default).
