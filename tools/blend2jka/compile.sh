#!/bin/bash
# jedi_room: WzMap (Windows build, through wine) BSP -> VIS -> LIGHT
export WINEDEBUG=-all WINEPREFIX=/root/wine/pfx
cd /home/claude/jroom/pk3
W=/home/claude/wzbuild/WzMap.exe
B='Z:\home\claude\jroom\gd'
M='Z:\home\claude\jroom\gd\base\maps\jedi_room.map'
wine $W -game ja -fs_basepath "$B" -meta -patchmeta "$M" > /home/claude/jroom/bsp.log 2>&1 || exit 1
wine $W -game ja -fs_basepath "$B" -vis -saveprt "$M" > /home/claude/jroom/vis.log 2>&1 || exit 2
wine $W -game ja -fs_basepath "$B" -light -fast -samples 2 -bounce 1 -patchshadows "$M" > /home/claude/jroom/light.log 2>&1 || exit 3
echo done
