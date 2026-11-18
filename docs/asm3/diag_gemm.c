/* diag_gemm a.bin b.bin c.bin N [tol]
 * Locate the corrupted tiles and test structural hypotheses:
 *   missing term  : C = ref - A[:,k]*B[k,:]
 *   extra term    : C = ref + A[:,k]*B[k,:]
 *   wrong k term  : C = ref - A[:,k]*B[k,:] + A[:,k']*B[k',:]
 *   column subset / row subset of a tile
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

static float *load(const char *p, int n)
{
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", p); exit(2); }
    float *b = malloc((size_t)n * 4);
    if (fread(b, 4, n, f) != (size_t)n) { fprintf(stderr, "short %s\n", p); exit(2); }
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: %s a b c N [tol]\n", argv[0]); return 2; }
    int N = atoi(argv[4]);
    double tol = argc > 5 ? atof(argv[5]) : 1e-4;
    float *A = load(argv[1], N * N), *B = load(argv[2], N * N), *C = load(argv[3], N * N);
    double *ref = malloc(sizeof(double) * N * N);
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j) {
            double acc = 0;
            for (int k = 0; k < N; ++k) acc += (double)A[i*N+k] * (double)B[k*N+j];
            ref[i*N+j] = acc;
        }
    int ncols = N / 16;
    int bad = 0;
    /* per-tile summary */
    for (int tr = 0; tr < ncols; ++tr)
        for (int tc = 0; tc < ncols; ++tc) {
            int cnt = 0, rows = 0, cols = 0, rowmask = 0, colmask = 0;
            for (int r = 0; r < 16; ++r) {
                int rb = 0;
                for (int c = 0; c < 16; ++c) {
                    int i = tr*16+r, j = tc*16+c;
                    double e = fabs((double)C[i*N+j] - ref[i*N+j]);
                    if (e > tol * (fabs(ref[i*N+j]) > 1.0 ? fabs(ref[i*N+j]) : 1.0)) {
                        ++cnt; rb = 1; colmask |= 1 << c;
                    }
                }
                if (rb) { ++rows; rowmask |= 1 << r; }
            }
            if (!cnt) continue;
            ++bad;
            int nc = 0; for (int c = 0; c < 16; ++c) nc += (colmask >> c) & 1;
            printf("TILE r=%d c=%d bad=%d/256 rows=0x%04x(rowmask) ncols=%d cols=0x%04x\n",
                   tr, tc, cnt, rowmask, nc, colmask);
            /* structural fits over the whole tile */
            double best[3] = {1e30, 1e30, 1e30};
            int bk[3] = {-1, -1, -1};
            for (int k = 0; k < N; ++k) {
                /* missing term */
                double res = 0;
                for (int r = 0; r < 16; ++r)
                    for (int c = 0; c < 16; ++c) {
                        int i = tr*16+r, j = tc*16+c;
                        double pred = -(double)A[i*N+k] * (double)B[k*N+j];
                        double d = ((double)C[i*N+j] - ref[i*N+j]) - pred;
                        if (fabs(d) > fabs(res)) res = d;
                    }
                if (fabs(res) < fabs(best[0])) { best[0] = res; bk[0] = k; }
                /* extra term */
                res = 0;
                for (int r = 0; r < 16; ++r)
                    for (int c = 0; c < 16; ++c) {
                        int i = tr*16+r, j = tc*16+c;
                        double pred = (double)A[i*N+k] * (double)B[k*N+j];
                        double d = ((double)C[i*N+j] - ref[i*N+j]) - pred;
                        if (fabs(d) > fabs(res)) res = d;
                    }
                if (fabs(res) < fabs(best[1])) { best[1] = res; bk[1] = k; }
            }
            printf("   fit missing k=%d maxres=%.4g | extra k=%d maxres=%.4g\n",
                   bk[0], best[0], bk[1], best[1]);
            printf("   err matrix (got-ref), rows x cols:\n");
            for (int r = 0; r < 16; ++r) {
                printf("    r%2d:", r);
                for (int c = 0; c < 16; ++c) {
                    int i = tr*16+r, j = tc*16+c;
                    printf(" %8.4f", (double)C[i*N+j] - ref[i*N+j]);
                }
                printf("\n");
            }
            printf("   ref matrix:\n");
            for (int r = 0; r < 16; ++r) {
                printf("    r%2d:", r);
                for (int c = 0; c < 16; ++c) {
                    int i = tr*16+r, j = tc*16+c;
                    printf(" %8.3f", ref[i*N+j]);
                }
                printf("\n");
            }
        }
    printf("DIAG N=%d badtiles=%d\n", N, bad);
    return bad ? 1 : 0;
}
