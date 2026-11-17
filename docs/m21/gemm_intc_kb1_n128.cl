#define N 128
__kernel void gemm_intc_kb1_128(__global const float* a, __global const float* b, __global float* c)
{
  int lane = get_global_id(0);
  int yid  = get_global_id(1);
  int blk  = yid / 32;
  int rg   = yid % 32;
  int c0   = blk * 64 + lane;
  int aBase = rg * (4 * N);
  float r0c0 = 0.0f, r0c1 = 0.0f, r0c2 = 0.0f, r0c3 = 0.0f, r1c0 = 0.0f, r1c1 = 0.0f, r1c2 = 0.0f, r1c3 = 0.0f, r2c0 = 0.0f, r2c1 = 0.0f, r2c2 = 0.0f, r2c3 = 0.0f, r3c0 = 0.0f, r3c1 = 0.0f, r3c2 = 0.0f, r3c3 = 0.0f;
  for (int k = 0; k < N; k += 1)
  {
    float a0_0 = a[aBase + 0 * N + k + 0];
    float a1_0 = a[aBase + 1 * N + k + 0];
    float a2_0 = a[aBase + 2 * N + k + 0];
    float a3_0 = a[aBase + 3 * N + k + 0];
    float b0_0 = b[(k + 0) * N + c0 + 0];
    float b0_1 = b[(k + 0) * N + c0 + 16];
    float b0_2 = b[(k + 0) * N + c0 + 32];
    float b0_3 = b[(k + 0) * N + c0 + 48];
    r0c0 += a0_0 * b0_0;
    r0c1 += a0_0 * b0_1;
    r0c2 += a0_0 * b0_2;
    r0c3 += a0_0 * b0_3;
    r1c0 += a1_0 * b0_0;
    r1c1 += a1_0 * b0_1;
    r1c2 += a1_0 * b0_2;
    r1c3 += a1_0 * b0_3;
    r2c0 += a2_0 * b0_0;
    r2c1 += a2_0 * b0_1;
    r2c2 += a2_0 * b0_2;
    r2c3 += a2_0 * b0_3;
    r3c0 += a3_0 * b0_0;
    r3c1 += a3_0 * b0_1;
    r3c2 += a3_0 * b0_2;
    r3c3 += a3_0 * b0_3;
  }
  c[aBase + 0 * N + c0 + 0] = r0c0;
  c[aBase + 0 * N + c0 + 16] = r0c1;
  c[aBase + 0 * N + c0 + 32] = r0c2;
  c[aBase + 0 * N + c0 + 48] = r0c3;
  c[aBase + 1 * N + c0 + 0] = r1c0;
  c[aBase + 1 * N + c0 + 16] = r1c1;
  c[aBase + 1 * N + c0 + 32] = r1c2;
  c[aBase + 1 * N + c0 + 48] = r1c3;
  c[aBase + 2 * N + c0 + 0] = r2c0;
  c[aBase + 2 * N + c0 + 16] = r2c1;
  c[aBase + 2 * N + c0 + 32] = r2c2;
  c[aBase + 2 * N + c0 + 48] = r2c3;
  c[aBase + 3 * N + c0 + 0] = r3c0;
  c[aBase + 3 * N + c0 + 16] = r3c1;
  c[aBase + 3 * N + c0 + 32] = r3c2;
  c[aBase + 3 * N + c0 + 48] = r3c3;
}
