#!/bin/bash
cd /home/claude/md/tools/retarget
for v in "out_reg vader_kenobi" "out_dmg vader_kenobi_broken"; do
  set -- $v
  python3 jka_retarget.py rig /home/claude/kv/$1/vader.glb --gla /home/claude/vbf2/in/hum/_humanoid_vader/_humanoid.gla \
    --map dooku_bf2_map.json --name $2 --tags /home/claude/vader_in/darthvader/model.glm --cloth-mesh cape --fingers --arm-ik \
    --relax-left --stock-hand-tags --ground-drop 2.6 --max-verts 4000 --max-tris 8000 \
    --lod /home/claude/kv/$1/vader_lod1.glb --lod /home/claude/kv/$1/vader_lod2.glb --lod /home/claude/kv/$1/vader_lod3.glb \
    --out /home/claude/kv/zzz_$2.pk3 2>&1 | grep -E "wrote|rror"
  mkdir -p /home/claude/kv/g_$2 && unzip -o -q -j /home/claude/kv/zzz_$2.pk3 "models/players/$2/model.glm" "models/players/_humanoid_$2/_humanoid.gla" "models/players/$2/model_default.skin" -d /home/claude/kv/g_$2
done
python3 -c "
from render import read_glm
import numpy as np
for n in ['vader_kenobi','vader_kenobi_broken']:
    s=[x for x in read_glm('/home/claude/kv/g_%s/model.glm'%n) if x['P'].size and not x['flags']&3 and '_off' not in x['name']]
    P=np.concatenate([x['P'] for x in s]); print(n, P.min(0).round(1), P.max(0).round(1))
"
grep -v _off /home/claude/kv/g_vader_kenobi_broken/model_default.skin
