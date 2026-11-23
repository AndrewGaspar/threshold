# tools/fg/sight-count.py RING: the Sight layer's Reflex frame counting over 2 s, read from its status block
# (qv_layer_status in the ring header, quiver-perf include/quiver_perf.h; run where the ring is, e.g. on rayblaster for /dev/shm/quiver-lab-1000/<inst>/quiver-1000/lab-<inst>-r<N>/layer-*.ring).
import struct, sys, time
def rd(p):
    b = open(p, 'rb').read(4096)
    o = 208
    pres, = struct.unpack_from('<Q', b, o + 48)
    flags, = struct.unpack_from('<I', b, o + 256)
    mk, sl, af, ap = struct.unpack_from('<QQQQ', b, o + 264)
    return pres, flags, mk, sl, af, ap
a = rd(sys.argv[1]); time.sleep(2); b = rd(sys.argv[1])
d = [(y - x) / 2 for x, y in zip(a, b)]
print(f"presents/s {d[0]:.1f}  reflex_flags {b[1]:#x}  markers/s {d[2]:.1f}  sleeps/s {d[3]:.1f}  "
      f"app frames/s {d[4]:.1f}  app presents/s {d[5]:.1f}  ratio {d[5]/d[4] if d[4] else 0:.2f}")
