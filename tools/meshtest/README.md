# Mesh cloth test harness

Runs the real tr_meshcloth.cpp on a .glm/.gla offline with game-like frame timing.

- `sh build.sh` (paths inside point at this repo; a non-zero exit with no output means it built).
- `harness model.glm _humanoid.gla meshcloth.cfg rec.bin` - prints stretch, displacement, clearance,
  arm penetration (ARMS), wrap-through (THRU) per animation segment, and cost.
  Env: `ANIMCFG=animation.cfg` (take frames from the model's own cfg), `FIGHT=1` (saber moves),
  `JUMP=1`, `IDLE=1` (10 s standing), `FPS=30`, `JIT=1`, `WALL="x y z nx ny nz"`, `VD=dist` (LOD),
  `SLEEP=0`, `BODYFIT=2` (fitted body shapes on top of the capsules), `NOSWEEP=1`, `LIGHTNAME=anim`.
- `recview.py model.glm gla rec.bin cape out.png t1,t2,...` - render recorded frames.
- `penetration.py model.glm gla rec.bin surfaces zfree` - cloth points behind the real body surface.
- `fitview.py` - plot fitted body shapes over the body (harness `FITDUMP=1` output).
