"""elbow bend (degrees, 0 = straight arm) per animation: Movie Duels' gla vs a retargeted one
usage: elbows.py md.gla new.gla animation.cfg ANIM [ANIM...]"""
import sys, numpy as np
from g2 import GLA
from render import skin_matrices


def joint_pos(g, S, name):
    i = g.byName[name.lower()]
    base = g.bones[i]['base']
    return (S[i] @ base)[:3, 3]


def elbow(g, f, side):
    S = skin_matrices(g, f)
    a = joint_pos(g, S, side + 'humerus'); b = joint_pos(g, S, side + 'radius'); c = joint_pos(g, S, side + 'hand')
    u = b - a; v = c - b
    return np.degrees(np.arccos(np.clip(np.dot(u, v) / np.linalg.norm(u) / np.linalg.norm(v), -1, 1)))


md, new = GLA(sys.argv[1]), GLA(sys.argv[2])
anims = {}
for line in open(sys.argv[3], errors='replace'):
    t = line.split()
    if len(t) >= 3 and not t[0].startswith('//'):
        try: anims[t[0].upper()] = (int(t[1]), int(t[2]))
        except ValueError: pass
for an in sys.argv[4:]:
    f0, n = anims[an]
    for f in (f0, f0 + n // 2):
        print('%-24s f%-6d  MD L %5.1f R %5.1f   new L %5.1f R %5.1f' % (an, f, elbow(md, f, 'l'), elbow(md, f, 'r'), elbow(new, f, 'l'), elbow(new, f, 'r')))
