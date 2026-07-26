/*
 * upscale — VGA → 4K-aspect-corrected RGBA upscaler.
 *
 * See upscale.h for the public API and the two-pass design rationale
 * (NN-expand-then-Lanczos preserves hard pixel-art edges horizontally
 * while applying anti-aliasing only on the vertical axis where the 6/5
 * aspect correction actually requires resampling).
 *
 * Performance note: the conceptual pipeline allocates an sw·9 × sh·11
 * intermediate (~50 MB for typical sources) then runs separable Lanczos
 * over it. We instead Lanczos directly off the source rows and replicate
 * 9× horizontally as the cheap final pass — both passes work in the
 * same RGBA byte format, no double scratch buffers.
 */

#include "upscale.h"

#include <math.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double sinc_pi(double x)
{
    if (x == 0.0) return 1.0;
    double xp = x * M_PI;
    return sin(xp) / xp;
}

static double lanczos_kernel(double x, int a)
{
    if (x <= -a || x >= a) return 0.0;
    return sinc_pi(x) * sinc_pi(x / (double)a);
}

static void premultiply_inplace(uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t a = p[i * 4 + 3];
        p[i * 4 + 0] = (uint8_t)((p[i * 4 + 0] * a + 127) / 255);
        p[i * 4 + 1] = (uint8_t)((p[i * 4 + 1] * a + 127) / 255);
        p[i * 4 + 2] = (uint8_t)((p[i * 4 + 2] * a + 127) / 255);
    }
}

static void unpremultiply_inplace(uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t a = p[i * 4 + 3];
        if (a == 0) continue;
        for (int c = 0; c < 3; c++) {
            int v = (p[i * 4 + c] * 255 + a / 2) / a;
            if (v > 255) v = 255;
            p[i * 4 + c] = (uint8_t)v;
        }
    }
}

int scale_y_4k(int v)
{
    return (int)((v * 54 + 2) / 5);
}

int scale_svga_xy_to_4k(int v)
{
    return (v * 9 + 1) / 2;
}

/* Single-axis Lanczos-3 resample over a "virtual" NN-expanded source
 * — `n_unique × by = total virtual rows`, where each src row maps to
 * `by` consecutive virtual rows. Used by both atlas_vga_to_4k (vertical
 * axis only, by=11) and atlas_svga_to_4k (both axes, by=5). The
 * virtual-NN trick lets us do the full Lanczos kernel against the
 * NN-expanded source without ever materialising the intermediate.
 *
 * src_stride / dst_stride are byte strides between rows on the major
 * axis. For Y resampling: stride is row width × 4. For X resampling:
 * call with src/dst transposed conceptually — the helper itself only
 * needs the major-axis stride. (We just keep two passes here for
 * clarity instead of a transpose.) */
static bool lanczos_axis_resample(const uint8_t *src, int sw, int sh,
                                  uint8_t *dst, int dw, int dh,
                                  int by, bool axis_y)
{
    /* axis_y = true  → resample the Y axis. sh = src rows, dh = dst rows.
     *                  src/dst rows are sw RGBA pixels.
     * axis_y = false → resample the X axis. sw = src cols, dw = dst cols.
     *                  src/dst rows are sh rows; we walk one dst column
     *                  at a time, reading along the row.
     */
    int N_src   = axis_y ? sh : sw;        /* source extent on the resampled axis */
    int N_dst   = axis_y ? dh : dw;        /* destination extent on the resampled axis */
    int N_other = axis_y ? sw : sh;        /* the other axis dim — unchanged */
    int N_mid   = N_src * by;              /* virtual NN-expanded extent */

    const int A = 3;
    double scale = (double)N_mid / (double)N_dst;
    double fscale = scale > 1.0 ? scale : 1.0;
    double radius = (double)A * fscale;

    int max_taps = (int)ceil(2.0 * radius) + 2;
    double *aggw    = (double *)malloc((size_t)max_taps * sizeof(double));
    int    *src_idx = (int    *)malloc((size_t)max_taps * sizeof(int));
    if (!aggw || !src_idx) { free(aggw); free(src_idx); return false; }

    for (int j = 0; j < N_dst; j++) {
        double cj = ((double)j + 0.5) * scale - 0.5;
        int j0 = (int)ceil(cj - radius);
        int j1 = (int)floor(cj + radius);
        if (j0 < 0)        j0 = 0;
        if (j1 >= N_mid)   j1 = N_mid - 1;

        /* Walk virtual rows, collapse runs of same src index. */
        int n_unique = 0;
        int prev_src = -1;
        double wsum = 0.0;
        for (int r_mid = j0; r_mid <= j1; r_mid++) {
            double w = lanczos_kernel((cj - r_mid) / fscale, A);
            int s = r_mid / by;
            if (s != prev_src) {
                src_idx[n_unique] = s;
                aggw[n_unique]    = w;
                n_unique++;
                prev_src = s;
            } else {
                aggw[n_unique - 1] += w;
            }
            wsum += w;
        }
        double iw = (wsum != 0.0) ? (1.0 / wsum) : 0.0;

        for (int i = 0; i < N_other; i++) {
            double r = 0, g = 0, b = 0, a = 0;
            for (int t = 0; t < n_unique; t++) {
                const uint8_t *p = axis_y
                    ? src + ((size_t)src_idx[t] * sw + i) * 4   /* row src_idx[t], col i */
                    : src + ((size_t)i * sw + src_idx[t]) * 4;  /* row i, col src_idx[t] */
                double wt = aggw[t];
                r += p[0] * wt; g += p[1] * wt;
                b += p[2] * wt; a += p[3] * wt;
            }
            r *= iw; g *= iw; b *= iw; a *= iw;
            uint8_t *q = axis_y
                ? dst + ((size_t)j * dw + i) * 4    /* row j, col i */
                : dst + ((size_t)i * dw + j) * 4;   /* row i, col j */
            q[0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r + 0.5);
            q[1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g + 0.5);
            q[2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b + 0.5);
            q[3] = (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a + 0.5);
        }
    }
    free(aggw); free(src_idx);
    return true;
}

bool atlas_svga_to_4k(uint8_t **inout_pixels, int *inout_w, int *inout_h)
{
    uint8_t *src = *inout_pixels;
    int sw = *inout_w, sh = *inout_h;
    int dw = scale_svga_xy_to_4k(sw);
    int dh = scale_svga_xy_to_4k(sh);
    const int by = 5;     /* NN expand factor — virtual; never materialised */

    premultiply_inplace(src, (size_t)sw * (size_t)sh);

    /* Pass 1 — Y axis: sw × sh → sw × dh, Lanczos against the
     * virtual sh*5-row source. */
    uint8_t *vmid = (uint8_t *)malloc((size_t)sw * (size_t)dh * 4);
    if (!vmid) return false;
    if (!lanczos_axis_resample(src, sw, sh, vmid, sw, dh, by, true)) {
        free(vmid); return false;
    }

    /* Pass 2 — X axis: sw × dh → dw × dh, Lanczos against the
     * virtual sw*5-col intermediate. */
    uint8_t *out = (uint8_t *)malloc((size_t)dw * (size_t)dh * 4);
    if (!out) { free(vmid); return false; }
    if (!lanczos_axis_resample(vmid, sw, dh, out, dw, dh, by, false)) {
        free(vmid); free(out); return false;
    }
    free(vmid);

    unpremultiply_inplace(out, (size_t)dw * (size_t)dh);

    free(src);
    *inout_pixels = out;
    *inout_w = dw;
    *inout_h = dh;
    return true;
}

bool atlas_vga_to_4k(uint8_t **inout_pixels, int *inout_w, int *inout_h)
{
    uint8_t *src = *inout_pixels;
    int sw = *inout_w, sh = *inout_h;
    int fw = sw * SCALE_X_4K;
    int fh = scale_y_4k(sh);
    const int by = 11;
    int mh = sh * by;

    premultiply_inplace(src, (size_t)sw * (size_t)sh);

    /* Vertical-pass intermediate: sw columns × fh rows, RGBA bytes. */
    uint8_t *vmid = (uint8_t *)malloc((size_t)sw * (size_t)fh * 4);
    if (!vmid) return false;

    const int A = 3;
    double scale = (double)mh / (double)fh;
    double fscale = scale > 1.0 ? scale : 1.0;
    double radius = (double)A * fscale;

    /* Per-output-row scratch: one entry per unique src row that any
     * kernel tap lands in. Worst case = every tap is a distinct row. */
    int max_taps = (int)ceil(2.0 * radius) + 2;
    double *aggw = (double *)malloc((size_t)max_taps * sizeof(double));
    int *src_rows = (int *)malloc((size_t)max_taps * sizeof(int));
    if (!aggw || !src_rows) {
        free(aggw); free(src_rows); free(vmid); return false;
    }

    for (int y = 0; y < fh; y++) {
        double cy = ((double)y + 0.5) * scale - 0.5;
        int y0 = (int)ceil(cy - radius);
        int y1 = (int)floor(cy + radius);
        if (y0 < 0) y0 = 0;
        if (y1 >= mh) y1 = mh - 1;

        /* Walk mid rows once, collapse runs that share a src row. We
         * iterate r_mid in ascending order so identical s values are
         * contiguous — single "did the bucket change" check, no map. */
        int n_unique = 0;
        int prev_src = -1;
        double wsum = 0.0;
        for (int r_mid = y0; r_mid <= y1; r_mid++) {
            double w = lanczos_kernel((cy - r_mid) / fscale, A);
            int s = r_mid / by;
            if (s != prev_src) {
                src_rows[n_unique] = s;
                aggw[n_unique] = w;
                n_unique++;
                prev_src = s;
            } else {
                aggw[n_unique - 1] += w;
            }
            wsum += w;
        }
        double iw = (wsum != 0.0) ? (1.0 / wsum) : 0.0;

        uint8_t *drow = vmid + (size_t)y * sw * 4;
        for (int x = 0; x < sw; x++) {
            double r = 0, g = 0, b = 0, a = 0;
            for (int t = 0; t < n_unique; t++) {
                const uint8_t *p = src +
                                   ((size_t)src_rows[t] * sw + x) * 4;
                double wt = aggw[t];
                r += p[0] * wt; g += p[1] * wt;
                b += p[2] * wt; a += p[3] * wt;
            }
            r *= iw; g *= iw; b *= iw; a *= iw;
            drow[x*4+0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r + 0.5);
            drow[x*4+1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g + 0.5);
            drow[x*4+2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b + 0.5);
            drow[x*4+3] = (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a + 0.5);
        }
    }
    free(aggw); free(src_rows);

    /* Horizontal NN expand: replicate each col 9× into final output.
     * Pure 32-bit byte copy, no arithmetic. */
    uint8_t *out = (uint8_t *)malloc((size_t)fw * (size_t)fh * 4);
    if (!out) { free(vmid); return false; }
    for (int y = 0; y < fh; y++) {
        const uint32_t *vrow = (const uint32_t *)(vmid +
                                                  (size_t)y * sw * 4);
        uint32_t *drow = (uint32_t *)(out + (size_t)y * fw * 4);
        for (int x = 0; x < sw; x++) {
            uint32_t px = vrow[x];
            uint32_t *q = drow + x * 9;
            q[0] = px; q[1] = px; q[2] = px;
            q[3] = px; q[4] = px; q[5] = px;
            q[6] = px; q[7] = px; q[8] = px;
        }
    }
    free(vmid);

    unpremultiply_inplace(out, (size_t)fw * (size_t)fh);

    free(src);
    *inout_pixels = out;
    *inout_w = fw;
    *inout_h = fh;
    return true;
}
