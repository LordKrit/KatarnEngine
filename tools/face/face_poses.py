"""Face pose recipes for BF2-style FACIAL_ rigs (Frostbite heads: Dooku, and other BF2 ports).
Head space at bind: +x = character's left, -y = forward, +z = up; units are game units
(this head: mouth 1.85 wide, lips ~0.75 tall). Mouth shapes use Rhubarb's letters."""

X = (1, 0, 0)
P = 'FACIAL_LOD1_'
LOWER = [P + r'C_LipLower', P + r'[LR]_LipLower', P + r'[LR]_LipLowerOuter', P + r'C_UnderChin', P + r'[LR]_UnderChin']
CORNERS = [P + r'[LR]_LipCorner']
UPPER = [P + r'C_LipUpper', P + r'[LR]_LipUpper', P + r'[LR]_LipUpperOuter']

def jaw(deg):
    # the jaw bone carries the chin and tongue; lower lip and under-chin follow it from the mouth bone
    return [('rot', [P + 'C_Jaw'], X, deg, P + 'C_Jaw'),
            ('rot', LOWER, X, deg, P + 'C_Jaw', 0.9),
            ('rot', CORNERS, X, deg, P + 'C_Jaw', 0.45)]

def side(fn, l, r):
    return [('move', [P + 'L_' + fn], l), ('move', [P + 'R_' + fn], (-r[0], r[1], r[2]))]

def corners(out=0.0, up=0.0, fwd=0.0):
    return side('LipCorner', (out, -fwd, up), (out, -fwd, up))

def lips_fwd(a):
    return [('move', UPPER + LOWER[:3], (0, -a, 0))]

BF2 = {
    # rest
    'X': [],
    # M B P: lips pressed together
    'A': [('move', UPPER, (0, 0, -0.06)), ('move', LOWER[:3], (0, -0.03, 0.10))] + corners(-0.03),
    # most consonants: teeth slightly apart
    'B': jaw(4) + corners(0.06),
    # E: open, corners wide
    'C': jaw(8) + corners(0.18, 0.03) + [('move', UPPER, (0, 0, 0.04))],
    # A (ah): wide open
    'D': jaw(15) + corners(0.08) + [('move', UPPER, (0, 0, 0.07))],
    # O: open and round
    'E': jaw(10) + corners(-0.22, 0, 0.10) + lips_fwd(0.16),
    # OO W: small and pushed out
    'F': jaw(4) + corners(-0.32, 0, 0.14) + lips_fwd(0.26),
    # F V: lower lip tucked under the upper teeth
    'G': jaw(3) + [('move', LOWER[:3], (0, 0.14, 0.16)), ('move', UPPER, (0, 0, 0.03))],
    # L: open, tongue up behind the teeth
    'H': jaw(8) + corners(0.06) + [('rot', [P + 'C_Tongue2'], X, -25, None),
                                  ('rot', [P + 'C_Tongue3'], X, -20, None)],
    # eyes shut
    'blink': [('rot', [P + '[LR]_EyelidUpper'], X, 48, None),
              ('rot', [P + '[LR]_EyelidLower'], X, -12, None)],
    # --- expressions
    'angry': side('ForeheadIn', (-0.12, -0.06, -0.30), (-0.12, -0.06, -0.30))
             + side('ForeheadMid', (0, 0, -0.16), (0, 0, -0.16))
             + side('EyesackLower', (0, 0, 0.07), (0, 0, 0.07))
             + side('NasolabialFurrow', (0, 0, 0.06), (0, 0, 0.06))
             + side('Nostril', (0.03, 0, 0.04), (0.03, 0, 0.04))
             + corners(0.05, -0.08) + [('move', UPPER, (0, 0, 0.04))]
             + [('rot', [P + '[LR]_EyelidUpper'], X, 8, None)],
    'smirk': side('LipCorner', (0.14, -0.02, 0.18), (0, 0, 0.02))
             + side('CheekLower', (0.03, 0, 0.10), (0, 0, 0))
             + side('NasolabialBulge', (0.03, -0.02, 0.06), (0, 0, 0))
             + side('EyesackLower', (0, 0, 0.04), (0, 0, 0)),
    'smile': corners(0.16, 0.16) + side('CheekLower', (0.03, 0, 0.10), (0.03, 0, 0.10))
             + side('NasolabialBulge', (0.03, -0.02, 0.06), (0.03, -0.02, 0.06))
             + side('EyesackLower', (0, 0, 0.05), (0, 0, 0.05)),
    'sneer': side('Nostril', (0.04, 0, 0.08), (0.02, 0, 0.03))
             + side('NasolabialFurrow', (0.02, 0, 0.12), (0, 0, 0.04))
             + side('LipUpperOuter', (0, 0, 0.12), (0, 0, 0.03))
             + side('ForeheadIn', (0, 0, -0.10), (0, 0, -0.10)),
    'sad': side('ForeheadIn', (0.02, 0, 0.14), (0.02, 0, 0.14)) + side('ForeheadOut', (0, 0, -0.06), (0, 0, -0.06))
           + corners(-0.03, -0.12) + [('rot', [P + '[LR]_EyelidUpper'], X, 10, None)],
    'pain': jaw(6) + side('ForeheadIn', (-0.06, 0, 0.10), (-0.06, 0, 0.10)) + corners(0.18, -0.06)
            + side('EyesackLower', (0, 0, 0.10), (0, 0, 0.10)) + [('move', UPPER, (0, 0, 0.08))]
            + [('rot', [P + '[LR]_EyelidUpper'], X, 20, None)],
    'surprise': jaw(7) + side('ForeheadIn', (0, 0, 0.18), (0, 0, 0.18)) + side('ForeheadMid', (0, 0, 0.20), (0, 0, 0.20))
                + side('ForeheadOut', (0, 0, 0.16), (0, 0, 0.16))
                + [('rot', [P + '[LR]_EyelidUpper'], X, -12, None)],
    'squint': [('rot', [P + '[LR]_EyelidUpper'], X, 14, None),
               ('rot', [P + '[LR]_EyelidLower'], X, -6, None)]
              + side('EyesackLower', (0, 0, 0.08), (0, 0, 0.08)) + side('ForeheadIn', (0, 0, -0.06), (0, 0, -0.06)),
}
