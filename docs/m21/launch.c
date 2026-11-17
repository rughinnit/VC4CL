/* launch.c - measure VC4CL enqueue/finish overhead.
 * usage: launch <mode> <reps>
 *   mode 0: enqueue 1 kernel, finish each rep
 *   mode 1: enqueue 10 kernels, one finish per batch (amortised)
 *   mode 2: enqueue 1, finish each, with a real 64x64 global store
 */
#define CL_TARGET_OPENCL_VERSION 120
#define _POSIX_C_SOURCE 200809L
#include <CL/cl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

int main(int argc, char** argv)
{
    int mode = argc > 1 ? atoi(argv[1]) : 0;
    int reps = argc > 2 ? atoi(argv[2]) : 20;
    const char* src = (mode == 2) ?
        "__kernel void k(__global float* c){ int g=get_global_id(0); c[g]=1.0f; }" :
        "__kernel void k(__global float* c){ }";

    cl_platform_id p;
    cl_device_id d;
    cl_int e;
    if(clGetPlatformIDs(1, &p, NULL) != CL_SUCCESS || clGetDeviceIDs(p, CL_DEVICE_TYPE_ALL, 1, &d, NULL) != CL_SUCCESS)
    {
        printf("no device\n");
        return 2;
    }
    cl_context ctx = clCreateContext(NULL, 1, &d, NULL, NULL, &e);
    cl_command_queue q = clCreateCommandQueue(ctx, d, 0, &e);
    cl_program prog = clCreateProgramWithSource(ctx, 1, &src, NULL, &e);
    e = clBuildProgram(prog, 1, &d, "", NULL, NULL);
    if(e != CL_SUCCESS)
    {
        char log[8192];
        clGetProgramBuildInfo(prog, d, CL_PROGRAM_BUILD_LOG, sizeof(log), log, NULL);
        printf("build fail %d: %s\n", e, log);
        return 2;
    }
    cl_kernel k = clCreateKernel(prog, "k", &e);
    cl_mem buf = clCreateBuffer(ctx, CL_MEM_READ_WRITE, 1 << 20, NULL, &e);
    clSetKernelArg(k, 0, sizeof(buf), &buf);

    size_t g1[1] = {1};
    size_t g64[1] = {4096};

    /* warm up */
    clEnqueueNDRangeKernel(q, k, 1, NULL, mode == 2 ? g64 : g1, NULL, 0, NULL, NULL);
    clFinish(q);

    double t0 = now_ms();
    for(int r = 0; r < reps; ++r)
    {
        if(mode == 1)
        {
            for(int i = 0; i < 10; ++i)
                clEnqueueNDRangeKernel(q, k, 1, NULL, g1, NULL, 0, NULL, NULL);
            clFinish(q);
        }
        else
        {
            clEnqueueNDRangeKernel(q, k, 1, NULL, mode == 2 ? g64 : g1, NULL, 0, NULL, NULL);
            clFinish(q);
        }
    }
    double ms = (now_ms() - t0) / reps;
    printf("mode=%d reps=%d per_rep=%.3f ms%s\n", mode, reps, ms, mode == 1 ? " (10 kernels)" : "");
    clReleaseMemObject(buf);
    clReleaseKernel(k);
    clReleaseProgram(prog);
    clReleaseCommandQueue(q);
    clReleaseContext(ctx);
    return 0;
}
