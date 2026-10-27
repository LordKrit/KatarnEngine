#!/bin/bash
# Rebuild both BF2 Vader models (twist joints now go to MD twist bones in the retarget).
cd /home/claude/vbf2
blender -b --python exp2.py 2>&1 | grep -E "CAPE lining|DONE|rror"
blender -b --python exp3.py 2>&1 | grep -E "CAPE lining|DONE|rror"
cd /home/claude/md/tools/retarget
for v in "out b vader_bf2" "outr br vader_bf2_rots"; do
  set -- $v
  python3 jka_retarget.py rig /home/claude/vbf2/$1/vader.glb --gla /home/claude/vbf2/in/hum/_humanoid_vader/_humanoid.gla \
    --map dooku_bf2_map.json --name $3 --tags /home/claude/vader_in/darthvader/model.glm --cloth-mesh cape --fingers --arm-ik \
    --relax-left --stock-hand-tags --ground-drop 2.6 --max-verts 4000 --max-tris 8000 \
    --lod /home/claude/vbf2/$1/vader_lod1.glb --lod /home/claude/vbf2/$1/vader_lod2.glb --lod /home/claude/vbf2/$1/vader_lod3.glb \
    --out /home/claude/vbf2/$2/zzz_$3.pk3 2>&1 | grep -E "wrote|rror"
done
# repack with the existing textures / shaders / npc
cd /home/claude/vbf2
for v in "pk b vader_bf2" "pkr br vader_bf2_rots"; do
  set -- $v
  (cd $1 && rm -rf models/players/_humanoid_$3 && unzip -o -q ../$2/zzz_$3.pk3 models/players/$3/model.glm models/players/$3/model_default.skin "models/players/_humanoid_$3/*" \
    && rm -f ../zzz_$3.pk3 ../zzz_$3_skel.pk3 && zip -q -r ../zzz_$3_skel.pk3 models/players/_humanoid_$3 && zip -q -r ../zzz_$3.pk3 models/players/$3 shaders ext_data \
    && cp ../zzz_$3.pk3 ../zzz_$3_skel.pk3 /mnt/user-data/outputs/)
done
ls -l /mnt/user-data/outputs/zzz_vader*
