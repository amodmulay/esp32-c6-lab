# heart

A beating heart at 65 bpm (lub-dub pulse). **Touch it** and it swells past the screen edges, the whole screen goes red, then it shrinks back; the whole thing takes 4 seconds. Touches during the animation are ignored.

```
python tools/flash.py flash heart
```

It draws into an off-screen buffer so there is no flicker. Tune `BPM`, `R_REST`, `GROW`/`HOLD`/`SHRINK` at the top of `heart.ino`.
