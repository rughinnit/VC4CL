/* check_gemm a.bin b.bin c.bin N [tol]
 * A,B,C raw fp32 row-major NxN.  Oracle: double accumulation in k order,
 * plus an fp32 accumulation in the same order (should be bit-exact). */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

static float *load(const char *p, int n)
{
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "check_gemm: cannot open %s\n", p); exit(2); }
    float *b = malloc((size_t)n * 4);
    if (fread(b, 4, n, f) != (size_t)n) { fprintf(stderr, "check_gemm: short %s\n", p); exit(2); }
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: %s a.bin b.bin c.bin N [tol]\n", argv[0]); return 2; }
    int N = atoi(argv[4]);
    double tol = argc > 5 ? atof(argv[5]) : 1e-4;
    float *A = load(argv[1], N * N), *B = load(argv[2], N * N), *C = load(argv[3], N * N);
    double maxabs = 0, maxrel = 0, maxc = 0;
    long mismatch = 0, bit_exact = 0, nonzero = 0;
    for (int i = 0; i < N; ++i)
    {
        for (int j = 0; j < N; ++j)
        {
            double acc = 0.0;
            float facc = 0.0f;
            for (int k = 0; k < N; ++k)
            {
                acc += (double)A[i * N + k] * (double)B[k * N + j];
                facc += A[i * N + k] * B[k * N + j];
            }
            double got = (double)C[i * N + j];
            double err = fabs(got - acc);
            double rel = err / (fabs(acc) > 1e-9 ? fabs(acc) : 1e-9);
            if (err > maxabs) maxabs = err;
            if (rel > maxrel) maxrel = rel;
            if (fabs(acc) > maxc) maxc = fabs(acc);
            if (err > tol * (fabs(acc) > 1.0 ? fabs(acc) : 1.0))
            {
                if (mismatch < 12)
                    printf("MISMATCH c[%d][%d] got %.9g ref %.9g err %.3g\n", i, j, got, acc, err);
                ++mismatch;
            }
            if (memcmp(&C[i * N + j], &facc, 4) == 0) ++bit_exact;
            if (C[i * N + j] != 0.0f) ++nonzero;
        }
    }
    printf("CHECK N=%d tol=%g maxabs=%.6g maxrel=%.3g max|ref|=%.4f mismatches=%ld/%d bit-exact_fp32=%ld/%d nonzero=%ld\n",
           N, tol, maxabs, maxrel, maxc, mismatch, N * N, bit_exact, N * N, nonzero);
    return mismatch == 0 ? 0 : 1;
}
