#!/bin/bash
# pack the compiled jedi_room into zz_jedi_room.pk3 (models, shaders, bsp + any external lightmaps)
set -e
cd /home/claude/jroom/pk3
OUT=/mnt/user-data/outputs/jedi_room
mkdir -p $OUT
rm -f $OUT/zz_jedi_room.pk3
zip -q -r -9 $OUT/zz_jedi_room.pk3 models/map_objects/jedi_room/*.png \
	shaders/jedi_room.shader maps/jedi_room.bsp \
	$(ls -d maps/jedi_room 2>/dev/null)
cp maps/jedi_room.map $OUT/jedi_room.map
ls -la $OUT
