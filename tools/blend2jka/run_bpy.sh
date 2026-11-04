#!/bin/bash
# bpy 5.2 (the .blend is Blender 5.2 format); clean env avoids a glog double-init abort
exec env -i PATH=/usr/bin:/bin HOME=/root /home/claude/bpyenv/bin/python -I "$@"
