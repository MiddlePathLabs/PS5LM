# Driver patches

Patches to PS5_Mesa's RADV, applied by `scripts/build-native-radv.sh` to an export of the pinned
Mesa revision (`1ba9874`), never to `.deps/PS5_Mesa` itself.

- `0001`: a destroyed buffer's memory waits until the GPU has run the work submitted before it.
  The PS5 winsys released it at once, so work already submitted could reach unmapped memory, and
  a GPU fault on the PS5 hangs the whole console. From PS5CEMU-HAR (`patches/mesa`, GPL-3.0) by
  way of cobanov/PS5LM v0.9.0 (`patches/mesa/0007`), unchanged.

Upstream's `0008` (a submit thread) and `0009` (wait spin and sleep) change nothing unless their
switches are set, and upstream measured no gain from them, so they are not taken.
