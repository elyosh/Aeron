/*
 * opt2gltf — convert one OPT file (or every OPT in a directory) to
 * glTF 2.0 + .bin + .png assets.
 *
 * Usage:
 *   opt2gltf [--scale F] [--smooth-angle DEG] [--emissive]
 *            <input.opt|input_dir> <output_dir>
 *
 * If the input is a directory, every *.OPT inside (case-insensitive)
 * is converted and emitted under <output_dir>/<basename>/. If a single
 * OPT file, the output goes directly into <output_dir>/.
 *
 * --scale F multiplies every vertex position (default 1.0). It exists to
 * reconcile OPTs authored at the full 1995 LFD coordinate scale, which
 * render 2x too large (the runtime scales OPT coords by 1/65536 vs the
 * classic FLIGHT_VERT_SCALE = 1/131072, so a correct OPT is half-scale).
 * Pass --scale 0.5 for such ships (e.g. FRTC, TUG).
 *
 * --smooth-angle F regenerates vertex normals from geometry with an
 * F-degree smoothing threshold. Stored vertex and face normals are ignored.
 * Connected smoothing fans are built across shared edges, then averaged
 * with corner-angle weights so triangulation density does not bias them;
 * wrapped/non-manifold fans split before a normal can point behind its
 * rendered triangle.
 * The default is OFF (negative) — the original 1998 normals (full-Gouraud,
 * no threshold) are kept, preserving the rounded look.
 * Stored normals reproduce the classic renderer's per-FaceData position
 * remap: the first face-order normal for a position is reused by later
 * corners, preventing dormant conflicting normals from becoming glTF
 * seams. Pass e.g. --smooth-angle 45 for crisp flat panels, or 90 for a
 * smoother low-poly look without accepting back-facing neighbours.
 *
 * --emissive exports self-illumination (lit windows, cockpit lights, etc.):
 * textures with self-lit texels get a second *_emissive.png wired as the
 * material's emissiveTexture. Off by default. For XWA (version 5) OPTs this
 * replicates the engine's lightmap derivation EXACTLY
 * (ModelTexture_FilterHardwarePalette @ 0x44A600 + CreateD3DfromTexture):
 * an entry is self-lit when its darkest palette row is bright (squared
 * 5-bit magnitude >= 32, green LSB dropped) and rows 1..6 stay within
 * squared distance 16 of row 0; glow color from palette row 10; textures
 * named "_*" and palettes where none/all 256 entries classify are skipped.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "opt.h"
#include "opt2gltf.h"

static int ends_with_icase(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lf = strlen(suffix);
    if (lf > ls) return 0;
    return strcasecmp(s + ls - lf, suffix) == 0;
}

static void strip_ext(char *dst, size_t cap, const char *fname)
{
    snprintf(dst, cap, "%s", fname);
    char *dot = strrchr(dst, '.');
    if (dot) *dot = '\0';
}

static int convert_one(const char *opt_path, const char *out_dir,
                       const char *basename, float vertex_scale,
                       float smooth_angle_deg, bool repair_normals,
                       bool emissive)
{
    opt_error_t err;
    opt_file_t *opt = opt_load_file(opt_path, &err);
    if (!opt) {
        fprintf(stderr, "opt2gltf: %s: %s\n", opt_path, err.msg);
        return 0;
    }
    int ok = opt2gltf_convert(opt, out_dir, basename, vertex_scale,
                              smooth_angle_deg, repair_normals,
                              emissive) ? 1 : 0;
    opt_free(opt);
    return ok;
}

int main(int argc, char **argv)
{
    float vertex_scale     = 1.0f;
    /* Default: keep the original 1998 OPT normals (full-Gouraud rounded
     * look). Pass --smooth-angle DEG to regenerate with a threshold. */
    float smooth_angle_deg = -1.0f;
    /* Classic position-normal remap + canonical-normal repair are on by
     * default (stored-normals mode); --no-normal-repair emits the source
     * per-corner data verbatim for diagnostics. */
    bool  repair_normals   = true;
    /* Self-illumination export is opt-in. */
    bool  emissive         = false;
    int ai = 1;
    /* Optional --scale / --smooth-angle flags, any order, before the
     * positional args. */
    while (ai < argc && argv[ai][0] == '-') {
        if (strcmp(argv[ai], "--scale") == 0) {
            if (ai + 1 >= argc) {
                fprintf(stderr, "opt2gltf: --scale needs a value\n");
                return 2;
            }
            vertex_scale = strtof(argv[ai + 1], NULL);
            if (!(vertex_scale > 0.0f)) {
                fprintf(stderr, "opt2gltf: --scale must be > 0\n");
                return 2;
            }
            ai += 2;
        } else if (strcmp(argv[ai], "--smooth-angle") == 0) {
            if (ai + 1 >= argc) {
                fprintf(stderr, "opt2gltf: --smooth-angle needs a value\n");
                return 2;
            }
            smooth_angle_deg = strtof(argv[ai + 1], NULL);
            ai += 2;
        } else if (strcmp(argv[ai], "--no-normal-repair") == 0) {
            repair_normals = false;
            ai += 1;
        } else if (strcmp(argv[ai], "--emissive") == 0) {
            emissive = true;
            ai += 1;
        } else {
            fprintf(stderr, "opt2gltf: unknown option '%s'\n", argv[ai]);
            return 2;
        }
    }
    if (argc - ai != 2) {
        fprintf(stderr,
            "usage: %s [--scale F] [--smooth-angle DEG] [--no-normal-repair] "
            "[--emissive] "
            "<input.OPT|input_dir> <output_dir>\n",
            argv[0]);
        return 2;
    }
    const char *in   = argv[ai];
    const char *outd = argv[ai + 1];

    struct stat st;
    if (stat(in, &st) != 0) {
        fprintf(stderr, "opt2gltf: cannot stat '%s': %s\n", in, strerror(errno));
        return 1;
    }

    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(in);
        if (!d) {
            fprintf(stderr, "opt2gltf: cannot open dir '%s'\n", in);
            return 1;
        }
        int ok_count = 0, fail_count = 0;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            if (!ends_with_icase(de->d_name, ".opt")) continue;
            char src[1024];
            snprintf(src, sizeof src, "%s/%s", in, de->d_name);
            char base[256];
            strip_ext(base, sizeof base, de->d_name);
            char out_sub[1024];
            snprintf(out_sub, sizeof out_sub, "%s/%s", outd, base);
            if (convert_one(src, out_sub, base, vertex_scale,
                            smooth_angle_deg, repair_normals,
                            emissive)) ++ok_count;
            else                                                       ++fail_count;
        }
        closedir(d);
        fprintf(stderr, "opt2gltf: %d ok, %d failed\n", ok_count, fail_count);
        return fail_count == 0 ? 0 : 1;
    }

    /* Single file. */
    const char *slash = strrchr(in, '/');
    const char *fname = slash ? slash + 1 : in;
    char base[256];
    strip_ext(base, sizeof base, fname);
    return convert_one(in, outd, base, vertex_scale,
                       smooth_angle_deg, repair_normals,
                       emissive) ? 0 : 1;
}
