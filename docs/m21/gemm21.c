/*
 * gemm19.c - M19 GEMM harness: 2D register tiling (rows and columns per work-item).
 *
 * Usage: gemm19 <source.cl> <kernel> <n> <rows> <cols> [reps] [localx localy]
 *
 * The global work size is (n/cols, n/rows). Work-item (gx, gy) computes the
 * rows x cols output tile at row gy*rows, column gx*cols.
 *
 * Every rep is verified element by element against a double-precision CPU
 * oracle; the mismatch count and max_rel of each rep are printed. Exit code:
 * 0 all reps PASS, 1 mismatch, 2 setup/build error.
 */
#define CL_TARGET_OPENCL_VERSION 120
#define _POSIX_C_SOURCE 200809L
#include <CL/cl.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

#define CL_CHECK(expr)                                                                              \
    do                                                                                              \
    {                                                                                               \
        cl_int _e = (expr);                                                                          \
        if(_e != CL_SUCCESS)                                                                         \
        {                                                                                            \
            printf("SETUP FAIL: %s -> %d\n", #expr, _e);                                             \
            fflush(stdout);                                                                          \
            return 2;                                                                                \
        }                                                                                            \
    } while(0)

int main(int argc, char** argv)
{
    if(argc < 6)
    {
        printf("usage: gemm19 <source.cl> <kernel> <n> <rows> <cols> [reps] [localx localy]\n");
        return 2;
    }
    const char* path = argv[1];
    const char* kname = argv[2];
    int n = atoi(argv[3]);
    int rows = atoi(argv[4]);
    int cols = atoi(argv[5]);
    int reps = argc > 6 ? atoi(argv[6]) : 3;
    int lx = argc > 7 ? atoi(argv[7]) : 0;
    int ly = argc > 8 ? atoi(argv[8]) : 0;

    if(n % rows || n % cols)
    {
        printf("SETUP FAIL: n=%d not divisible by rows=%d cols=%d\n", n, rows, cols);
        return 2;
    }

    FILE* f = fopen(path, "rb");
    if(!f)
    {
        printf("cannot open %s\n", path);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* src = (char*)malloc((size_t)len + 1);
    if(fread(src, 1, (size_t)len, f) != (size_t)len)
    {
        printf("short read on %s\n", path);
        return 2;
    }
    src[len] = 0;
    fclose(f);

    cl_platform_id plat;
    cl_device_id dev;
    cl_uint num;
    CL_CHECK(clGetPlatformIDs(1, &plat, &num));
    CL_CHECK(clGetDeviceIDs(plat, CL_DEVICE_TYPE_GPU, 1, &dev, &num));
    cl_context ctx = clCreateContext(NULL, 1, &dev, NULL, NULL, NULL);
    cl_command_queue q = clCreateCommandQueue(ctx, dev, 0, NULL);

    size_t msize = (size_t)n * (size_t)n * sizeof(float);
    float* a = (float*)malloc(msize);
    float* b = (float*)malloc(msize);
    float* c = (float*)calloc((size_t)n * (size_t)n, sizeof(float));
    double* ref = (double*)malloc((size_t)n * (size_t)n * sizeof(double));
    unsigned seed = 12345u;
    for(int i = 0; i < n * n; ++i)
    {
        seed = seed * 1103515245u + 12345u;
        a[i] = (float)((seed >> 16) % 2000) / 1000.0f - 1.0f;
        seed = seed * 1103515245u + 12345u;
        b[i] = (float)((seed >> 16) % 2000) / 1000.0f - 1.0f;
    }
    /* double oracle */
    for(int i = 0; i < n; ++i)
    {
        for(int j = 0; j < n; ++j)
        {
            double s = 0.0;
            for(int k = 0; k < n; ++k)
                s += (double)a[(size_t)i * n + k] * (double)b[(size_t)k * n + j];
            ref[(size_t)i * n + j] = s;
        }
    }

    cl_mem da = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, msize, a, NULL);
    cl_mem db = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, msize, b, NULL);
    cl_mem dc = clCreateBuffer(ctx, CL_MEM_READ_WRITE, msize, NULL, NULL);

    const char* sources[1] = {src};
    const char* bopts = getenv("GEMM19_OPTS");
    if(!bopts)
        bopts = "";
    printf("build options: '%s'\n", bopts);
    cl_program prog = clCreateProgramWithSource(ctx, 1, sources, NULL, NULL);
    cl_int err = clBuildProgram(prog, 1, &dev, bopts, NULL, NULL);
    {
        char log[32768];
        clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, sizeof(log), log, NULL);
        if(log[0])
            printf("build log: %s\n", log);
    }
    if(err != CL_SUCCESS)
    {
        printf("BUILD FAIL %s: err=%d\n", kname, err);
        return 2;
    }
    cl_kernel kern = clCreateKernel(prog, kname, NULL);
    if(!kern)
    {
        printf("cannot create kernel '%s'\n", kname);
        return 2;
    }
    CL_CHECK(clSetKernelArg(kern, 0, sizeof(cl_mem), &da));
    CL_CHECK(clSetKernelArg(kern, 1, sizeof(cl_mem), &db));
    CL_CHECK(clSetKernelArg(kern, 2, sizeof(cl_mem), &dc));

    size_t gsize[2] = {(size_t)16, (size_t)((n / rows) * (n / 64))}; /* int map: x=lane, y=rowgroup*colblock */
    if(getenv("GEMM19_PADY"))
        gsize[1] = (size_t)atoi(getenv("GEMM19_PADY"));
    if(getenv("GEMM19_PADX"))
        gsize[0] = (size_t)atoi(getenv("GEMM19_PADX"));
    size_t lsize[2] = {(size_t)lx, (size_t)ly};
    printf("grid %zux%zu local %zux%zu\n", gsize[0], gsize[1], lsize[0], lsize[1]);

    double best = 1e30, worst = 0.0;
    int allbad = 0;
    for(int r = 0; r < reps; ++r)
    {
        /* sentinel: zero the output before every rep so an unwritten element is detected */
        memset(c, 0, msize);
        CL_CHECK(clEnqueueWriteBuffer(q, dc, CL_TRUE, 0, msize, c, 0, NULL, NULL));
        double t0 = now_ms();
        CL_CHECK(clEnqueueNDRangeKernel(q, kern, 2, NULL, gsize,
                                        (lx || ly) ? lsize : NULL, 0, NULL, NULL));
        CL_CHECK(clFinish(q));
        double ms = now_ms() - t0;
        if(ms < best)
            best = ms;
        if(ms > worst)
            worst = ms;

        CL_CHECK(clEnqueueReadBuffer(q, dc, CL_TRUE, 0, msize, c, 0, NULL, NULL));
        double maxRel = 0.0;
        int bad = 0, first = -1;
        int shown = 0;
        for(int i = 0; i < n; ++i)
        {
            for(int j = 0; j < n; ++j)
            {
                size_t idx = (size_t)i * n + j;
                double s = ref[idx];
                double got = (double)c[idx];
                double rel = fabs(s - got) / (1.0 + fabs(s));
                if(rel > maxRel)
                    maxRel = rel;
                if(rel > 1e-3)
                {
                    if(first < 0)
                        first = (int)idx;
                    if(shown < 6)
                    {
                        printf("  BAD r=%d c=%d got=%.9g ref=%.9g rel=%.3e\n", i, j, got, s, rel);
                        ++shown;
                    }
                    ++bad;
                }
            }
        }
        allbad += bad;
        double gf = 2.0 * (double)n * (double)n * (double)n / (ms * 1e6);
        printf("RESULT %s n=%d rows=%d cols=%d rep=%d ms=%.4f GFLOP/s=%.4f max_rel=%.3e "
               "mismatch=%d first_bad=%d %s\n",
               kname, n, rows, cols, r, ms, gf, maxRel, bad, first, bad ? "FAIL" : "PASS");
        fflush(stdout);
    }
    double gf = 2.0 * (double)n * (double)n * (double)n / (best * 1e6);
    printf("SUMMARY %s n=%d rows=%d cols=%d best=%.4f worst=%.4f GFLOP/s=%.4f total_mismatch=%d %s\n",
           kname, n, rows, cols, best, worst, gf, allbad, allbad ? "FAIL" : "PASS");
    clReleaseMemObject(da);
    clReleaseMemObject(db);
    clReleaseMemObject(dc);
    clReleaseKernel(kern);
    clReleaseProgram(prog);
    clReleaseCommandQueue(q);
    clReleaseContext(ctx);
    return allbad ? 1 : 0;
}
