#!/usr/bin/env python3
"""M21d: nat-map 4x4 tile with vector stores (vstore4) so the merge gate sees
a 16-byte element and a 16-byte per-lane step.

usage: gen21d.py <N> <kernel> <R> <C> <KB> <vecb:0|1>
"""
import sys

N, KERNEL = int(sys.argv[1]), sys.argv[2]
R, C, KB = int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
vecb = len(sys.argv) > 6 and sys.argv[6] == "1"
assert C == 4, "vstore4 needs C=4"

body = []
body.append("  int c0 = get_global_id(0) * %d;" % C)
body.append("  int aBase = get_global_id(1) * (%d * N);" % R)
accs = ["r%dc%d" % (i, j) for i in range(R) for j in range(C)]
body.append("  float " + ", ".join("%s = 0.0f" % a for a in accs) + ";")
body.append("  for (int k = 0; k < N; k += %d)" % KB)
body.append("  {")

for i in range(R):
    if KB % 4 == 0:
        body.append("    float4 av%d = vload4(0, a + aBase + %d * N + k);" % (i, i))
for i in range(R):
    for kk in range(KB):
        if KB % 4 == 0:
            body.append("    float a%d_%d = av%d.%s;" % (i, kk, i, "xyzw"[kk % 4]))
        else:
            body.append("    float a%d_%d = a[aBase + %d * N + k + %d];" % (i, kk, i, kk))
for kk in range(KB):
    if vecb:
        body.append("    float4 bv%d = vload4(0, b + (k + %d) * N + c0);" % (kk, kk))
    else:
        for j in range(C):
            body.append("    float b%d_%d = b[(k + %d) * N + c0 + %d];" % (kk, j, kk, j))
for i in range(R):
    for kk in range(KB):
        for j in range(C):
            bname = ("bv%d.%s" % (kk, "xyzw"[j])) if vecb else ("b%d_%d" % (kk, j))
            body.append("    %s += a%d_%d * %s;" % (accs[i * C + j], i, kk, bname))
body.append("  }")
for i in range(R):
    body.append("  vstore4((float4)(%s), 0, c + aBase + %d * N + c0);"
                % (", ".join("r%dc%d" % (i, j) for j in range(C)), i))
body.append("}")

hdr = ["#define N %d" % N,
       "__kernel void %s(__global const float* a, __global const float* b, __global float* c)" % KERNEL,
       "{"]
print("\n".join(hdr + body))
