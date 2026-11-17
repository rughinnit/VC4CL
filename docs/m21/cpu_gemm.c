/* cpu_gemm.c - 4x Cortex-A53 reference for the VC4 GEMM comparison.
 * usage: cpu_gemm <n> [reps] [threads]
 * row-major sgemm, jki loop, -O3 -mcpu=cortex-a53 -fopenmp. Verifies one element set
 * against a double oracle and reports GFLOP/s = 2*n^3/t.
 */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _OPENMP
#include <omp.h>
#endif

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

int main(int argc, char** argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 128;
    int reps = argc > 2 ? atoi(argv[2]) : 5;
    int threads = argc > 3 ? atoi(argv[3]) : 4;
#ifdef _OPENMP
    omp_set_num_threads(threads);
#endif
    size_t nn = (size_t)n * (size_t)n;
    float* a = malloc(nn * sizeof(float));
    float* b = malloc(nn * sizeof(float));
    float* c = malloc(nn * sizeof(float));
    double* ref = malloc(nn * sizeof(double));
    if(!a || !b || !c || !ref)
    {
        printf("alloc failed\n");
        return 2;
    }
    for(size_t i = 0; i < nn; ++i)
    {
        a[i] = (float)((int)(i * 7 % 19) - 9) * 0.25f;
        b[i] = (float)((int)(i * 11 % 23) - 11) * 0.125f;
    }
    for(int i = 0; i < n; ++i)
        for(int j = 0; j < n; ++j)
        {
            double s = 0.0;
            for(int k = 0; k < n; ++k)
                s += (double)a[(size_t)i * n + k] * (double)b[(size_t)k * n + j];
            ref[(size_t)i * n + j] = s;
        }

    double best = 1e30;
    int bad = 0;
    for(int r = 0; r < reps; ++r)
    {
        memset(c, 0, nn * sizeof(float));
        double t0 = now_ms();
#pragma omp parallel for schedule(static)
        for(int i = 0; i < n; ++i)
        {
            for(int k = 0; k < n; ++k)
            {
                const float aik = a[(size_t)i * n + k];
                const float* brow = b + (size_t)k * n;
                float* crow = c + (size_t)i * n;
                for(int j = 0; j < n; ++j)
                    crow[j] += aik * brow[j];
            }
        }
        double ms = now_ms() - t0;
        if(ms < best)
            best = ms;
        bad = 0;
        for(size_t i = 0; i < nn; ++i)
            if(fabs((double)c[i] - ref[i]) / (1.0 + fabs(ref[i])) > 1e-3)
                ++bad;
        printf("RESULT cpu_sgemm n=%d rep=%d ms=%.4f GFLOP/s=%.4f mismatch=%d %s\n", n, r, ms,
               2.0 * (double)n * (double)n * (double)n / (ms * 1e6), bad, bad ? "FAIL" : "PASS");
    }
    printf("SUMMARY cpu_sgemm n=%d threads=%d best=%.4f GFLOP/s=%.4f\n", n, threads, best,
           2.0 * (double)n * (double)n * (double)n / (best * 1e6));
    return bad ? 1 : 0;
}
