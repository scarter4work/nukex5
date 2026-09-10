// ── NukeX v4: Kernel 4 — Spatial Context ────────────────────────
// One work-item per pixel. Global size = width * height.
//
// Computes Sobel gradient magnitude, local background (biweight location),
// and local RMS (neighbour differences) over a 15×15 window.
//
// Operates on the stacked output image (channel-by-channel layout),
// NOT on voxels. This runs once after all voxel processing is complete.
//
// Matches gpu_cpu_fallback.cpp::spatial_context() exactly.

// Include common helpers
// (prepended by kernel compilation system)

#define WINDOW_RADIUS 7
#define MAX_WINDOW_SIZE 225  // (2*7+1)^2

__kernel void spatial_context(
    __global const float* stacked_image,    // [C * W * H]
    int width,
    int height,
    int n_channels,
    __global float* gradient_mag,           // [W * H]
    __global float* local_background,       // [W * H]
    __global float* local_rms               // [W * H]
) {
    int gid = get_global_id(0);
    if (gid >= width * height) return;

    int x = gid % width;
    int y = gid / width;
    int W = width;
    int H = height;
    int C = n_channels;

    // ── Sobel gradient (max across channels) ──
    float max_grad = 0.0f;
    for (int ch = 0; ch < C; ch++) {
        __global const float* img = stacked_image + ch * W * H;

        int xm = max(0, x - 1), xp = min(W - 1, x + 1);
        int ym = max(0, y - 1), yp = min(H - 1, y + 1);

        float gx = -img[ym * W + xm] + img[ym * W + xp]
                  - 2.0f * img[y * W + xm] + 2.0f * img[y * W + xp]
                  - img[yp * W + xm] + img[yp * W + xp];

        float gy = -img[ym * W + xm] - 2.0f * img[ym * W + x] - img[ym * W + xp]
                  + img[yp * W + xm] + 2.0f * img[yp * W + x] + img[yp * W + xp];

        float g = sqrt(gx * gx + gy * gy);
        if (g > max_grad) max_grad = g;
    }
    gradient_mag[gid] = max_grad;

    // ── Local background and RMS (15×15 window, biweight) ──
    float window[MAX_WINDOW_SIZE];
    int wn = 0;

    int y0 = max(0, y - WINDOW_RADIUS);
    int y1 = min(H - 1, y + WINDOW_RADIUS);
    int x0 = max(0, x - WINDOW_RADIUS);
    int x1 = min(W - 1, x + WINDOW_RADIUS);

    for (int wy = y0; wy <= y1; wy++) {
        for (int wx = x0; wx <= x1; wx++) {
            float lum;
            if (C >= 3) {
                lum = 0.2126f * stacked_image[0 * W * H + wy * W + wx]
                    + 0.7152f * stacked_image[1 * W * H + wy * W + wx]
                    + 0.0722f * stacked_image[2 * W * H + wy * W + wx];
            } else {
                lum = stacked_image[wy * W + wx];
            }
            window[wn++] = lum;
        }
    }

    // Biweight location (iterative, matches CPU fallback)
    float c_bw = 6.0f;
    int max_iter = 10;
    float tol = 1.0e-7f;

    float sorted_win[MAX_WINDOW_SIZE];
    for (int i = 0; i < wn; i++) sorted_win[i] = window[i];
    insertion_sort_f(sorted_win, wn);
    float location = sorted_median_f(sorted_win, wn);

    // MAD of window
    float abs_devs_win[MAX_WINDOW_SIZE];
    for (int i = 0; i < wn; i++)
        abs_devs_win[i] = fabs(window[i] - location);
    insertion_sort_f(abs_devs_win, wn);
    float mad_val = sorted_median_f(abs_devs_win, wn);

    if (mad_val > 1.0e-30f) {
        float scale = mad_val * 1.4826f;

        for (int iter = 0; iter < max_iter; iter++) {
            float num = 0.0f, den = 0.0f;
            for (int i = 0; i < wn; i++) {
                float u = (window[i] - location) / (c_bw * scale);
                if (fabs(u) < 1.0f) {
                    float u2 = u * u;
                    float w = (1.0f - u2) * (1.0f - u2);
                    num += w * window[i];
                    den += w;
                }
            }
            if (den < 1.0e-30f) break;
            float new_loc = num / den;
            if (fabs(new_loc - location) < tol * scale) {
                location = new_loc;
                break;
            }
            location = new_loc;
        }
    }

    local_background[gid] = location;

    // ── Local RMS from neighbour differences (mirrors the CPU fallback) ──
    //
    // MAD about the local median reports smooth STRUCTURE as noise: on a
    // noiseless ramp of slope s it returns 5.93*s. Differencing pixels a
    // fixed lag apart cancels anything smooth. The window is gathered
    // row-major.
    // Lag 8, both directions: adjacent pixels are correlated by debayering
    // and resampling (measured rho 0.34 mono, 0.6-0.7 OSC), and the
    // difference-based sigma flattens near lag 8. Mirrors the CPU fallback.
    const int LAG = 8;
    int win_w = x1 - x0 + 1;
    int win_h = y1 - y0 + 1;
    float dh[MAX_WINDOW_SIZE], dv[MAX_WINDOW_SIZE];
    int nh = 0, nv = 0;
    for (int ry = 0; ry < win_h; ry++)
        for (int rx = 0; rx + LAG < win_w; rx++)
            dh[nh++] = window[ry * win_w + rx + LAG] - window[ry * win_w + rx];
    for (int rx = 0; rx < win_w; rx++)
        for (int ry = 0; ry + LAG < win_h; ry++)
            dv[nv++] = window[(ry + LAG) * win_w + rx] - window[ry * win_w + rx];

    // Each direction is centred on its OWN median before pooling: a smooth
    // ramp gives one constant difference per direction, different between
    // directions, so pooling raw differences would read the ramp as scatter.
    float rms_val = 0.0f;
    if (nh + nv > 1) {
        float abs_dev[MAX_WINDOW_SIZE];
        int na = 0;
        float sorted[MAX_WINDOW_SIZE];
        if (nh > 0) {
            for (int i = 0; i < nh; i++) sorted[i] = dh[i];
            insertion_sort_f(sorted, nh);
            float m = sorted_median_f(sorted, nh);
            for (int i = 0; i < nh; i++) abs_dev[na++] = fabs(dh[i] - m);
        }
        if (nv > 0) {
            for (int i = 0; i < nv; i++) sorted[i] = dv[i];
            insertion_sort_f(sorted, nv);
            float m = sorted_median_f(sorted, nv);
            for (int i = 0; i < nv; i++) abs_dev[na++] = fabs(dv[i] - m);
        }
        insertion_sort_f(abs_dev, na);
        // 1.4826 converts MAD to a Gaussian sigma; 1/sqrt(2) undoes the
        // differencing of two independent samples.
        rms_val = sorted_median_f(abs_dev, na) * 1.4826f * 0.70710678f;
    }
    local_rms[gid] = rms_val;
}
