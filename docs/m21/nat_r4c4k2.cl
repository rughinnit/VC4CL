#define N 128
__kernel void gemm_nat_r4c4k2(__global const float* a, __global const float* b, __global float* c)
{
  int c0 = get_global_id(0) * 4;
  int aBase = get_global_id(1) * (4 * N);
  float r0c0 = 0.0f, r0c1 = 0.0f, r0c2 = 0.0f, r0c3 = 0.0f, r1c0 = 0.0f, r1c1 = 0.0f, r1c2 = 0.0f, r1c3 = 0.0f, r2c0 = 0.0f, r2c1 = 0.0f, r2c2 = 0.0f, r2c3 = 0.0f, r3c0 = 0.0f, r3c1 = 0.0f, r3c2 = 0.0f, r3c3 = 0.0f;
  for (int k = 0; k < N; k += 2)
  {
    float a0_0 = a[aBase + 0 * N + k + 0];
    float a0_1 = a[aBase + 0 * N + k + 1];
    float a1_0 = a[aBase + 1 * N + k + 0];
    float a1_1 = a[aBase + 1 * N + k + 1];
    float a2_0 = a[aBase + 2 * N + k + 0];
    float a2_1 = a[aBase + 2 * N + k + 1];
    float a3_0 = a[aBase + 3 * N + k + 0];
    float a3_1 = a[aBase + 3 * N + k + 1];
    float b0_0 = b[(k + 0) * N + c0 + 0];
    float b0_1 = b[(k + 0) * N + c0 + 1];
    float b0_2 = b[(k + 0) * N + c0 + 2];
    float b0_3 = b[(k + 0) * N + c0 + 3];
    float b1_0 = b[(k + 1) * N + c0 + 0];
    float b1_1 = b[(k + 1) * N + c0 + 1];
    float b1_2 = b[(k + 1) * N + c0 + 2];
    float b1_3 = b[(k + 1) * N + c0 + 3];
    r0c0 += a0_0 * b0_0;
    r0c1 += a0_0 * b0_1;
    r0c2 += a0_0 * b0_2;
    r0c3 += a0_0 * b0_3;
    r0c0 += a0_1 * b1_0;
    r0c1 += a0_1 * b1_1;
    r0c2 += a0_1 * b1_2;
    r0c3 += a0_1 * b1_3;
    r1c0 += a1_0 * b0_0;
    r1c1 += a1_0 * b0_1;
    r1c2 += a1_0 * b0_2;
    r1c3 += a1_0 * b0_3;
    r1c0 += a1_1 * b1_0;
    r1c1 += a1_1 * b1_1;
    r1c2 += a1_1 * b1_2;
    r1c3 += a1_1 * b1_3;
    r2c0 += a2_0 * b0_0;
    r2c1 += a2_0 * b0_1;
    r2c2 += a2_0 * b0_2;
    r2c3 += a2_0 * b0_3;
    r2c0 += a2_1 * b1_0;
    r2c1 += a2_1 * b1_1;
    r2c2 += a2_1 * b1_2;
    r2c3 += a2_1 * b1_3;
    r3c0 += a3_0 * b0_0;
    r3c1 += a3_0 * b0_1;
    r3c2 += a3_0 * b0_2;
    r3c3 += a3_0 * b0_3;
    r3c0 += a3_1 * b1_0;
    r3c1 += a3_1 * b1_1;
    r3c2 += a3_1 * b1_2;
    r3c3 += a3_1 * b1_3;
  }
  c[aBase + 0 * N + c0 + 0] = r0c0;
  c[aBase + 0 * N + c0 + 1] = r0c1;
  c[aBase + 0 * N + c0 + 2] = r0c2;
  c[aBase + 0 * N + c0 + 3] = r0c3;
  c[aBase + 1 * N + c0 + 0] = r1c0;
  c[aBase + 1 * N + c0 + 1] = r1c1;
  c[aBase + 1 * N + c0 + 2] = r1c2;
  c[aBase + 1 * N + c0 + 3] = r1c3;
  c[aBase + 2 * N + c0 + 0] = r2c0;
  c[aBase + 2 * N + c0 + 1] = r2c1;
  c[aBase + 2 * N + c0 + 2] = r2c2;
  c[aBase + 2 * N + c0 + 3] = r2c3;
  c[aBase + 3 * N + c0 + 0] = r3c0;
  c[aBase + 3 * N + c0 + 1] = r3c1;
  c[aBase + 3 * N + c0 + 2] = r3c2;
  c[aBase + 3 * N + c0 + 3] = r3c3;
}
