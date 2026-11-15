// Radiance cascades over the depth buffer: the light the visible surfaces send each other, at long range (about 60% of
// the picture's height at 16:9), without noise, at a fixed cost; its one history is a short blend of the bounce with
// last frame's at the probes, against shimmer in motion (AC_RC_TEMPORAL, below). It is the gi level of the
// ambient occlusion (vk_ao.c, ao = gi): the occlusion runs as at high, and these passes run between its light sent out
// and its blur, on its view z (ao_prep.comp) and that light (ao_src.comp, the light target without the fog, with
// coarser levels). Only with the game's ambient light known (vk_ao.c frame_light): without it the level is high alone.
// AC_GPU_PASSES times them on the rows "ao step 6" (the cascades) and "ao step 7" (the resolve).
//
// The technique is the screen-space ("flatland") hierarchy Sannikov describes for Path of Exile 2 in "Radiance
// Cascades: A Novel Approach to Calculating Global Illumination": probes on a screen grid, directions that are screen
// angles, intervals that are screen distances. From one cascade to the next the probe spacing doubles each way, the
// directions double and the interval doubles in length, so each cascade marches half the rays of the one below. One
// march per (probe, direction) serves every elevation, because the depth buffer is a height field: the direction gives
// a half-plane through the probe's view ray (ao.comp's slice), cut into 32 angular sectors (a bit each, the visibility
// bitmask of Therrien, Levesque and Gilet, "Screen Space Indirect Lighting with Visibility Bitmask") grouped into 4
// elevation bins. rc_trace.comp marches one cascade and merges the one above it, from the top cascade down to cascade
// 0; rc_resolve.comp turns cascade 0 into g, the bounce a probe receives over the game's ambient light along its normal
// (a uniform light L over the hemisphere gives L / ambient). Their headers give the details.
// Three rules without which the result was wrong, all measured on captured frames against a brute-force oracle (1024
// directions per probe, every texel up to the same reach, same model): a depth sample is a point of its ray with the
// depth interpolated (with the nearest texel's centre a grazing ground read 63 degrees where it runs at 105 and hid
// whole bins); a surface seen from behind hides but sends no light (without the test the reference's bounce went from
// 0.41 to 0.72 and from 0.17 to 0.42 on the two frames: foreground objects lit what stands behind them); and a sample
// farther than RC_REACH from the probe in view space neither lights nor hides (the sky is geometry in these scenes, up
// to 7300 units away, and would count as a source doubling the ambient light).
//
// Which light counts (RC_LIGHT, AC_RC_LIGHT): a surface the occlusion leaves open already receives the game's ambient
// light, so by default (above) a sample sends only what it gives beyond the sky it hides, max(L - sky along the
// direction to it, 0): an obstacle lit by the same sky changes nothing and the bounce never darkens. zero counts the
// samples' whole light, which counts the sky twice where nothing hides it and so brightens and tints everything with
// the sky's colour.
// The picture: ao_blur.comp brings g from the probes to the occlusion's texels (depth-aware, as ao_apply.frag does
// with the factor), caps it at RC_CAP, scales it by RC_STRENGTH, fades it out between RC_FADE0 and RC_FADE1 view units
// (the probes are least steady far away: measured on the oracle, g changes from one frame to the next by 0.018 within
// 5 units, 0.043 from 10 to 20, 0.078 from 20 to 40), and makes the factor on the light 1 + share x (g - (1 - k)) in
// place of 1 - share x (1 - k), share being the part of the pixel the sky gives (ao_blur.comp). ao_apply.frag lets the
// factor rise to 1 + RC_CAP x RC_STRENGTH. A black pixel stays black: the light is added in proportion to the pixel's
// own ambient part, with no albedo known. AC_RC_APPLY=0 computes the cascades without applying them (the picture of
// high).
//
// Measured (Arc 140T, Harmony Square in free play and on its title card, a 1472x828 scene under FSR 3 balanced for
// 2560x1440, medians of AC_GPU_PASSES). In the study, the occlusion at low and nothing applied: 0.69 ms for the cascades
// and 0.05 ms for the resolve with the constants below, 9.3 MiB for the two atlases (9.5 MiB with g's image); 4
// directions at cascade 0 in place of 8 0.42 + 0.04 ms (4.7 MiB of atlases), 5 cascades 0.64 + 0.05 ms, 4 steps 0.49
// + 0.05 ms, a probe every 2 texels 1.83 + 0.37 ms (37.4 MiB). The shaders give the oracle's result on the frame they
// ran on (ratio of the means 0.998 and 1.004, correlation 0.994 and 0.997). Against the brute-force reference the
// cascades lose about 20% of the light (mean x0.80 and x0.77; mean absolute error of g capped at 1, over the
// reference's mean, 0.39 and 0.48), mostly from the back-face test made on sparse samples (0.28 and 0.27 without it on
// both sides); the error lies at the far zones and silhouettes. From one frame to the next the brute-force reference
// itself changes by 22% at rest (the scene is jittered and a probe on one texel changes surface at silhouettes), the
// cascades by 26%.
// At the gi level as built here, nothing applied yet (the occlusion at high, same scene, one run each): 0.68 ms + 0.05
// ms, the shaders against the oracle 0.999 and 1.002 for the ratio of the means, 0.993 and 0.997 for the correlation;
// at 2560x1440 without an upscaler (spacing 7) the shaders' agreement with the oracle was measured too (ratio of the
// means 1.006 and 0.995, correlation 0.994 and 0.998) and gi cost about 1.03 ms over high; the picture was judged at
// 1472x828 only. Applied (variant above), gi cost 0.83 to 0.93 ms over high in single runs at 1472x828 (Arc 140T, FSR
// 3 or XeSS balanced for 2560x1440): about 0.70 ms the cascades, 0.06 ms the resolve, 0.09 ms more in the blur reading
// g (it stops early on open texels only beyond the fade); 1.2 to 1.4 ms at 2560x1440 without an upscaler.
// Not measured: another GPU, the steadiness of the applied bounce in motion.
// Left out after trials on the oracle (same error measure, the two frames): probes with directions on the sphere marched
// through the depth buffer (5 to 30 ms at 720p on an RTX 3060 by Kung's thesis), four times the directions and
// intervals per cascade (0.40 and 0.52 for more rays), a pyramid of the nearest depth (0.45 and 0.54: obstacles grow),
// a finite thickness of 0.5 behind each sample (0.44 and 0.59), merging by the product of the open shares (0.40 and
// 0.50, and an open share 0.107 off the reference's in place of 0.080), 64 sectors (no gain), a bin's light spread over
// the whole bin in place of its far end (0.39 to 0.44 on the first frame), and a probe every 2 texels (0.34 and 0.45
// for four times the rays). One elevation bin is unusable (0.76 and 0.75), two poor (0.44 and 0.54); eight gain
// little (0.37 and 0.47). The steps count more than the directions (2, 4, 8, 16 steps: 0.57, 0.48, 0.39, 0.33 on the
// first frame; 4, 8, 16 directions: 0.40, 0.39, 0.39). The cascades do not replace ao.comp: at a probe every 4
// occlusion texels they cannot draw a contact.
//
// Steadiness in motion: play testing saw a slight shimmer of the bounce a lamp casts on the ground while the camera
// moves (none at off, low or high, none in a still picture). Measured on two bursts of 8 consecutive frames captured
// at that spot in play (XeSS balanced, variant above), g compared probe by probe after the camera's motion is taken
// out: g moves by 19 to 25% of its mean from one frame to the next, most near small bright sources (a thin emissive strip
// 35 times the ambient light, whose bounce on the deck is a soft patch that shifts between frames). The oracle extended
// to the variant above gives the shaders' g on those frames (ratio of the means 1.004, correlation 0.9985) and the same
// change, so remedies were tried offline on the 14 pairs. The brute-force reference (every texel up to the same reach,
// 256 directions, no cascades) changes almost as much: in the patch 0.042 against 0.049 for the cascades, over the
// frame 0.030 against 0.033 (absolute change of g capped at 1). Most of the shimmer is therefore in the screen-space
// model under the camera's motion and the upscaler's jitter (what the depth buffer and the light show at a texel), not in
// the cascades' sampling, and no sampling change removes it: twice the directions -11% (the still picture moves by 0.02
// of the ambient light on average), the light read at the larger of the step and the gap between directions -3% to -11%
// (1x to 2x that footprint), a 4-tap tent over the light's levels -5%, one level coarser 0%, twice the steps -6% (13%
// more light), a sample's light capped at 16 times the sky 0%, no back-face test 0% in absolute terms (twice the light).
// Earlier, on a turning camera in Harmony Square, the 3x3 average of g between probes took 22% off the probes' change
// (another measure: the probes, not the patch), with no visible effect in play.
// The history (AC_RC_TEMPORAL, on by default at gi; no settings row, AC_RC_TEMPORAL=0 turns it off as a debug switch)
// is the remedy that works, outside the design's first rule (no history): rc_temporal.comp blends this frame's g with last frame's history at cascade 0's probes (RC_TEMPORAL_ALPHA),
// reprojected with the camera (whose matrices carry no jitter: the point lands up to half a scene pixel off, an eighth
// of a probe) and kept per history probe where its depth matches the plane through the probe (a slanted ground keeps
// its history to the fade's end; offline, 84 to 97% of the probes kept one, against 68 to 79% when all 4 had to match
// the reprojected depth, which dropped to 3% at 10 to 15 view units on a turning camera), from the last frame that ran,
// at the same grid, without a camera cut; ao_blur.comp then applies the history. Offline on those bursts it took 70%
// of the change off in the shimmering patch; in game on a turning camera in Harmony Square (XeSS, 8 consecutive
// frames) the probes' change fell from 0.23-0.33 to 0.05-0.06 of mean g, the mean unchanged, for 0.02 ms (Arc 140T, a
// 184x104 probe grid) and 0.3 MiB (two histories), and it passed play testing; hence on by default. Its price: the
// camera's motion alone reprojects it, so the bounce that something moving casts (a car, a character) follows it
// about 1 / weight = 3 frames late, a lag not measured on a fast object (a probe on the moving thing itself loses its
// history when its depth no longer matches); the game's own velocity buffer would remove that and is not used. Its
// pipeline and images are made at the first frame that runs the cascades, so nothing of it exists below gi; with
// AC_RC_TEMPORAL=0 they are never made and its pass never runs (nor while a debug view is shown): g of this frame
// alone.
//
// Debug views (AC_RC_DEBUG, no settings row; at ao = gi): g shows g in place of the light (white: as much light as
// the ambient light), open the open share of the hemisphere the cascades see (an occlusion of long reach: the check of
// their geometry). AC_RC_DEBUG_CASCADE=<0..5> with either traces that cascade alone, without those above it, and shows
// it; without a view it changes nothing. They go through ao_apply.frag's debug path (its blend off), by rc_view.comp
// into the occlusion's factor image, and nothing is applied while one is shown.
#include "vk_rc.h"

#include <math.h>
#include <string.h>

#include "host.h"
#include "rc_resolve_spv.h"
#include "rc_temporal_spv.h"
#include "rc_trace_spv.h"
#include "rc_view_spv.h"

#define RC_FORMAT VK_FORMAT_R16G16B16A16_SFLOAT   // storage and filtering mandatory; the light reaches 770 here
#define RC_BINS 4u              // image layers per cascade: rc_trace.comp's K elevation bins
#define RC_CASCADES 6           // reach RC_SPACING_MIN x 63 = 252 occlusion texels at 736x414, 61% of the height;
                                // 5 cascades (124 texels) saved 0.05 ms
#define RC_DIRS 8               // directions of cascade 0; 4 saved 0.27 ms, the error barely moved (0.40 against 0.39)
#define RC_STEPS 8              // depth samples per interval; 4 saved 0.20 ms for 0.48 against 0.39 of error
#define RC_SPACING_MIN 4        // probe spacing of cascade 0 in occlusion texels; 2 cost 2.20 ms in all
#define RC_PROBES_X 184         // cascade 0's probes across at most (the 736-wide occlusion of the measurements)
#define RC_REACH 40.0f          // view units beyond which a sample neither lights nor hides; 20 or 80 changed little
#define RC_DEPTH_TOL 2.0f       // the merge's depth tolerance: (4% of |z| + 0.05) x this x 2^cascade (rc_trace.comp)
#define RC_OFF_PRESENTS 600u    // presents without the gi level before the images are given back (vk_dlaa.c's rule),
                                // and after a failed creation before it is tried again
// The bounce on the picture (ao_blur.comp), the values of the offline simulation the plan proposed: g capped at as much
// light as the ambient light, at full strength, faded out from 25 to 50 view units. Not tuned in play.
#define RC_CAP 1.0f
#define RC_STRENGTH 1.0f
#define RC_FADE0 25.0f
#define RC_FADE1 50.0f
// AC_RC_TEMPORAL: the weight of this frame's g against the history (rc_temporal.comp). Offline on two bursts of 8
// consecutive frames captured in play at a shimmering spot (XeSS balanced), the frame-to-frame change of the result
// fell in the patch by 53% at 0.5, 70% at 0.3, 79% at 0.2, the history standing 0.014, 0.020, 0.024 of the ambient
// light from this frame's g; a real change of light shows through within about 1 / weight frames.
#define RC_TEMPORAL_ALPHA 0.3f
// A camera cut drops the history: vk_dlaa.c mv_pass's test and limits (it runs only with an upscaler, so it is
// repeated here on the same camera block): turned, moved and zoomed, each as its share of its limit, summing over 1.
#define RC_CUT_DEG 30.0
#define RC_CUT_DIST 5.0
#define RC_CUT_ZOOM 2.0

// The hierarchy for an occlusion of w x h. Spacing and first interval are counted in occlusion texels, so a larger
// occlusion (2560x1440 without an upscaler has 1280x720) would cost 3 times more at the same spacing: the spacing grows
// with the occlusion's width to keep cascade 0 near RC_PROBES_X probes across, and the first interval with it. The reach
// (63 first intervals) therefore follows the width alone: about 61% of the height at 16:9, but more in heights on a
// wider picture (about 87% at 3440x1440 without an upscaler, 128% at 32:9), and below 736 texels wide the floor of 4
// makes it reach beyond the picture's height. This rule is proposed: the shaders' agreement with the oracle and the
// cost are measured at 736x414 (spacing 4) and 1280x720 (spacing 7), the picture judged at 736x414 only.
typedef struct {
    uint32_t spacing, first;            // cascade 0's probe spacing and interval length
    int32_t grid[RC_CASCADES][4];       // per cascade: probes across, down, direction tiles across, down
    float start[RC_CASCADES + 1];       // interval bounds in occlusion texels
    uint32_t aw, ah;                    // the atlas: the largest cascade's tiles
} Hierarchy;

static void hierarchy(uint32_t w, uint32_t h, Hierarchy *H) {
    uint32_t sp = (w + RC_PROBES_X - 1) / RC_PROBES_X;
    H->spacing = sp > RC_SPACING_MIN ? sp : RC_SPACING_MIN;
    H->first = H->spacing;
    H->aw = H->ah = 0;
    H->start[0] = 0;
    for (int i = 0; i < RC_CASCADES; i++) {
        uint32_t s = H->spacing << i;
        int d = RC_DIRS << i, tx = 1;
        while (tx * tx < d) tx *= 2;   // tiles: a power of two across, the rest down
        H->grid[i][0] = (int32_t)((w + s - 1) / s), H->grid[i][1] = (int32_t)((h + s - 1) / s);
        H->grid[i][2] = tx, H->grid[i][3] = d / tx;
        uint32_t cw = (uint32_t)(H->grid[i][0] * tx), ch = (uint32_t)(H->grid[i][1] * (d / tx));
        if (cw > H->aw) H->aw = cw;
        if (ch > H->ah) H->ah = ch;
        H->start[i + 1] = H->start[i] + (float)(H->first << i);
    }
}

// rc_trace.comp's, rc_resolve.comp's and rc_view.comp's constants.
typedef struct {
    float ndc_to_view[4], size[4], cas[4];
    int32_t grid[4], up[4];
    float par[4], world[2][4];
} TracePush;
typedef struct { float ndc_to_view[4], size[4], cas[4]; int32_t grid[4]; float world[3][4]; } ResolvePush;
typedef struct { float size[4]; int32_t grid[4]; } ViewPush;
typedef struct { float ndc_to_view[4], prev_n2v[4], size[4], m[3][4]; int32_t grid[4]; } TemporalPush;

static struct {
    int tried, ok;
    VkPipeline trace, resolve, view, temporal;
    // the two atlases, used in turn (cascade i writes i & 1 and reads the other: cascade 0 ends in atlas 0), and g at
    // cascade 0's probes
    VkImage img[3];
    VmaAllocation mem[3];
    VkImageView iv[3];
    uint32_t w, h;              // the occlusion size they were made for
    uint32_t idle;              // presents without the gi level
    // AC_RC_TEMPORAL: two histories at cascade 0's probes, used in turn, and last frame's camera
    VkImage himg[2];
    VmaAllocation hmem[2];
    VkImageView hiv[2];
    int hcur, hok;              // the history last written; whether it holds last frame's
    uint32_t presents, hstamp;  // presents so far; the present of the last history written
    uint32_t hretry;            // presents left before a failed history is tried again
    int temporal_tried;         // the history's pipeline was asked for (at the first frame at gi with the history on)
    float prev_view[3][4], prev_n2v[4], prev_focal;
    uint32_t retry;             // presents left before a failed creation for fail_w x fail_h is tried again
    uint32_t fail_w, fail_h;
    // the last run: the cascade resolved into g's image, its spacing and probes
    uint32_t spacing;
    int32_t gw, gh;
    VkImageView bounce;         // what ao_blur.comp applies: g, or its history
    int logs, fail_logs;
} R;

static void free_images(int deferred) {
    VkCtx *c = vk_ctx();
    for (int i = 0; i < 3; i++) {
        if (deferred) {
            vk_defer_view(R.iv[i]);
            if (R.img[i]) vk_defer_image(R.img[i], R.mem[i]);
        } else {
            if (R.iv[i]) vkDestroyImageView(c->device, R.iv[i], NULL);
            if (R.img[i]) vmaDestroyImage(c->vma, R.img[i], R.mem[i]);
        }
        R.img[i] = VK_NULL_HANDLE, R.iv[i] = VK_NULL_HANDLE, R.mem[i] = NULL;
    }
    R.w = R.h = 0;
    for (int i = 0; i < 2; i++) {
        if (deferred) {
            vk_defer_view(R.hiv[i]);
            if (R.himg[i]) vk_defer_image(R.himg[i], R.hmem[i]);
        } else {
            if (R.hiv[i]) vkDestroyImageView(c->device, R.hiv[i], NULL);
            if (R.himg[i]) vmaDestroyImage(c->vma, R.himg[i], R.hmem[i]);
        }
        R.himg[i] = VK_NULL_HANDLE, R.hiv[i] = VK_NULL_HANDLE, R.hmem[i] = NULL;
    }
    R.hok = 0;
}

// The two histories at cascade 0's probes, made at the first frame at gi with the history on and freed with the images above
// (so remade with them when the grid changes). 0: none.
static int ensure_history(VkCommandBuffer cb, const Hierarchy *H) {
    if (R.himg[0]) return 1;
    VkCtx *c = vk_ctx();
    VkImageMemoryBarrier2 ib[2];
    for (int i = 0; i < 2; i++) {
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = RC_FORMAT, .extent = {(uint32_t)H->grid[0][0], (uint32_t)H->grid[0][1], 1},
                                .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                    .format = RC_FORMAT, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vmaCreateImage(c->vma, &ii, &ai, &R.himg[i], &R.hmem[i], NULL) != VK_SUCCESS) R.himg[i] = VK_NULL_HANDLE;
        vi.image = R.himg[i];
        if (!R.himg[i] || vkCreateImageView(c->device, &vi, NULL, &R.hiv[i]) != VK_SUCCESS) {
            R.hiv[i] = VK_NULL_HANDLE;
            for (int k = 0; k <= i; k++) {
                vk_defer_view(R.hiv[k]);
                if (R.himg[k]) vk_defer_image(R.himg[k], R.hmem[k]);
                R.himg[k] = VK_NULL_HANDLE, R.hiv[k] = VK_NULL_HANDLE, R.hmem[k] = NULL;
            }
            return 0;
        }
        ib[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = R.himg[i],
                                        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    }
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 2,
                           .pImageMemoryBarriers = ib};
    vkCmdPipelineBarrier2(cb, &di);
    R.hok = 0;
    return 1;
}

// Whether the camera cut between last frame and this one (vk_dlaa.c mv_pass's test, on rows 0-2 of the view).
static int camera_cut(const RcInputs *in) {
    double tr = 0, dist2 = 0, pc[3] = {0, 0, 0}, pp[3] = {0, 0, 0};
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) tr += (double)R.prev_view[r][k] * in->view[r][k];
    for (int k = 0; k < 3; k++)
        for (int r = 0; r < 3; r++)
            pc[k] -= (double)in->view[r][k] * in->view[r][3], pp[k] -= (double)R.prev_view[r][k] * R.prev_view[r][3];
    for (int k = 0; k < 3; k++) dist2 += (pc[k] - pp[k]) * (pc[k] - pp[k]);
    double c = (tr - 1) / 2, deg = acos(c > 1 ? 1 : c < -1 ? -1 : c) * 57.29577951308232;
    double zoom = in->focal > 0 && R.prev_focal > 0 ? fabs(log2((double)in->focal / R.prev_focal)) : 0;
    return deg / RC_CUT_DEG + sqrt(dist2) / RC_CUT_DIST + zoom / RC_CUT_ZOOM > 1.0;
}

// The atlases and g for an occlusion of w x h; a new size retires the old ones. New images go to GENERAL. A failed
// creation (a card short of memory) is not tried again for the same size before RC_OFF_PRESENTS presents: the level
// is high meanwhile.
static int ensure_images(VkCommandBuffer cb, uint32_t w, uint32_t h, const Hierarchy *H) {
    if (R.img[0] && R.w == w && R.h == h) return 1;
    if (R.retry && R.fail_w == w && R.fail_h == h) return 0;
    if (R.img[0]) free_images(1);
    VkCtx *c = vk_ctx();
    VkImageMemoryBarrier2 ib[3];
    for (int i = 0; i < 3; i++) {
        uint32_t layers = i < 2 ? RC_BINS : 1, iw = i < 2 ? H->aw : (uint32_t)H->grid[0][0],
                 ih = i < 2 ? H->ah : (uint32_t)H->grid[0][1];
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = RC_FORMAT, .extent = {iw, ih, 1}, .mipLevels = 1, .arrayLayers = layers,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        int ok = vmaCreateImage(c->vma, &ii, &ai, &R.img[i], &R.mem[i], NULL) == VK_SUCCESS;
        if (!ok) R.img[i] = VK_NULL_HANDLE;
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = R.img[i],
                                    .viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
                                    .format = RC_FORMAT,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}};
        if (ok && vkCreateImageView(c->device, &vi, NULL, &R.iv[i]) != VK_SUCCESS) R.iv[i] = VK_NULL_HANDLE, ok = 0;
        if (!ok) {
            free_images(1);
            R.retry = RC_OFF_PRESENTS, R.fail_w = w, R.fail_h = h;
            return 0;
        }
        ib[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = R.img[i],
                                        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}};
    }
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 3,
                           .pImageMemoryBarriers = ib};
    vkCmdPipelineBarrier2(cb, &di);
    R.w = w, R.h = h, R.retry = 0;
    if (R.logs++ < 8)
        rt_log("vk: indirect light (radiance cascades) on a %ux%u occlusion: %d cascades, probes every %u texels "
               "(%dx%d), %d directions, %d steps, first interval %u texels, reach %.0f texels, %.1f MiB",
               w, h, RC_CASCADES, H->spacing, H->grid[0][0], H->grid[0][1], RC_DIRS, RC_STEPS, H->first,
               H->start[RC_CASCADES],
               ((double)H->aw * H->ah * RC_BINS * 2 + (double)H->grid[0][0] * H->grid[0][1]) * 8.0 / 1048576.0);
    return 1;
}

static VkWriteDescriptorSet write(uint32_t b, VkDescriptorType t, const VkDescriptorImageInfo *info) {
    return (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = b,
                                  .descriptorCount = 1, .descriptorType = t, .pImageInfo = info};
}

int vk_rc_run(VkExec *ex, const RcInputs *in, int view, int cascade) {
    if (!R.tried) {   // at the first frame at the gi level: off, low and high never make them
        R.tried = 1;
        R.trace = vk_ao_comp_pipeline(rc_trace_spv, sizeof rc_trace_spv, in->lay, 0);
        R.resolve = vk_ao_comp_pipeline(rc_resolve_spv, sizeof rc_resolve_spv, in->lay, 0);
        R.view = vk_ao_comp_pipeline(rc_view_spv, sizeof rc_view_spv, in->lay, 0);
        R.ok = R.trace && R.resolve && R.view;
        if (!R.ok) rt_log("vk: indirect light off: its compute pipelines could not be created");
    }
    if (!R.ok) return 0;
    Hierarchy H;
    hierarchy(in->w, in->h, &H);
    if (!ensure_images(ex->cb, in->w, in->h, &H)) {
        if (R.fail_logs++ < 4)
            rt_log("vk: indirect light: its images for a %ux%u occlusion could not be created: high alone, tried again "
                   "in %u presents", in->w, in->h, RC_OFF_PRESENTS);
        return 0;
    }
    int top = RC_CASCADES - 1, bottom = 0;
    if (view != RC_VIEW_NONE && cascade >= 0) top = bottom = cascade < RC_CASCADES ? cascade : RC_CASCADES - 1;
    const VkDescriptorType S = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, I = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    VkDescriptorImageInfo z_in = {in->nearest, in->z, VK_IMAGE_LAYOUT_GENERAL},
                          src_in = {in->trilinear, in->src, VK_IMAGE_LAYOUT_GENERAL},
                          amb_in = {in->linear, in->amb, VK_IMAGE_LAYOUT_GENERAL};
    float sky = in->light == RC_LIGHT_ABOVE ? in->amb_factor : 0.0f;
    const float (*m)[4] = in->world;
    float det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    float det_sign = det < 0 ? -1.0f : 1.0f;
    for (int i = top; i >= bottom; i--) {
        TracePush p = {.size = {(float)in->w, (float)in->h, in->aspect, (float)(in->src_levels - 1)},
                       .cas = {(float)(H.spacing << i), (float)(RC_DIRS << i), H.start[i], H.start[i + 1]},
                       .par = {(float)RC_STEPS, RC_REACH, RC_DEPTH_TOL * (float)(1 << i), sky}};
        memcpy(p.ndc_to_view, in->ndc_to_view, sizeof p.ndc_to_view);
        memcpy(p.grid, H.grid[i], sizeof p.grid);
        memcpy(p.world, in->world, sizeof p.world);   // the first two rows
        // the third row's direction from them: their cross product times the sign of the determinant, so a mirrored
        // camera still reads the cube map the right way (TracePush has no room for the third row: its only free
        // floats are world0.w and world1.w, par.w being the sky's factor)
        p.world[0][3] = det_sign;
        if (i < top) memcpy(p.up, H.grid[i + 1], sizeof p.up);   // probes across 0 at the top: nothing to merge
        // the top cascade still binds the other atlas, unread: the binding must be valid
        VkDescriptorImageInfo o = {VK_NULL_HANDLE, R.iv[i & 1], VK_IMAGE_LAYOUT_GENERAL},
                              up = {in->linear, R.iv[(i + 1) & 1], VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[5] = {write(0, S, &z_in), write(1, I, &o), write(2, S, &src_in), write(3, S, &up),
                                     write(4, S, &amb_in)};
        vk_ao_dispatch(ex, 0, R.trace, w, 5, &p, sizeof p, ((uint32_t)(H.grid[i][0] * H.grid[i][2]) + 7) / 8,
                       ((uint32_t)(H.grid[i][1] * H.grid[i][3]) + 7) / 8);
    }
    if (vk_gpuprof_on()) vk_gpuprof_mark(ex, 6, 6);
    // cascade `bottom` to g (or to its open share), at its probes
    ResolvePush r = {.size = {(float)in->w, (float)in->h, view == RC_VIEW_OPEN ? 1.0f : 0.0f, 0},
                     .cas = {(float)(H.spacing << bottom), (float)(RC_DIRS << bottom), in->amb_factor, 0}};
    memcpy(r.ndc_to_view, in->ndc_to_view, sizeof r.ndc_to_view);
    memcpy(r.grid, H.grid[bottom], sizeof r.grid);
    memcpy(r.world, in->world, sizeof r.world);
    VkDescriptorImageInfo g_out = {VK_NULL_HANDLE, R.iv[2], VK_IMAGE_LAYOUT_GENERAL},
                          c0 = {in->nearest, R.iv[bottom & 1], VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[4] = {write(0, S, &z_in), write(1, I, &g_out), write(2, S, &c0), write(3, S, &amb_in)};
    vk_ao_dispatch(ex, 7, R.resolve, wr, 4, &r, sizeof r, ((uint32_t)H.grid[bottom][0] + 7) / 8,
                   ((uint32_t)H.grid[bottom][1] + 7) / 8);
    R.spacing = H.spacing << bottom, R.gw = H.grid[bottom][0], R.gh = H.grid[bottom][1];
    R.bounce = R.iv[2];
    // the history (AC_RC_TEMPORAL, never with a debug view): kept only from the frame just before, at the same grid,
    // without a camera cut; else this frame's g starts it
    if (in->temporal && view == RC_VIEW_NONE && !R.temporal && !R.temporal_tried) {   // at its first use only
        R.temporal_tried = 1;
        R.temporal = vk_ao_comp_pipeline(rc_temporal_spv, sizeof rc_temporal_spv, in->lay, 0);
        if (!R.temporal) rt_log("vk: indirect light: the history's pipeline could not be created: no history");
    }
    int hist = in->temporal && view == RC_VIEW_NONE && R.temporal && !R.hretry;
    if (hist && !ensure_history(ex->cb, &H)) {   // a card short of memory: tried again after RC_OFF_PRESENTS presents
        R.hretry = RC_OFF_PRESENTS, hist = 0;
        if (R.fail_logs++ < 4) rt_log("vk: indirect light: no memory for the history: tried again in %u presents",
                                      RC_OFF_PRESENTS);
    }
    if (hist) {
        // kept from the last frame that ran, one present apart or two (a scene drawn at 30 per second is presented
        // twice at 60), at the same grid and without a camera cut
        int keep = R.hok && R.presents - R.hstamp <= 2 && !camera_cut(in);
        // this frame's view to last frame's: V_prev V_cur^-1. The camera block's view is rigid: rows 0-2 times rows
        // 12-14 (its inverse) gave the identity within 1e-4 on captured frames, so V^-1 = [R^T | -R^T t]
        float m[3][4];
        for (int r = 0; r < 3; r++) {
            for (int k = 0; k < 3; k++) {
                double v = 0;
                for (int j = 0; j < 3; j++) v += (double)R.prev_view[r][j] * in->view[k][j];
                m[r][k] = (float)v;
            }
            double t = R.prev_view[r][3];
            for (int k = 0; k < 3; k++) t -= (double)m[r][k] * in->view[k][3];
            m[r][3] = (float)t;
        }
        TemporalPush tp = {.size = {(float)in->w, (float)in->h, (float)H.spacing, keep ? RC_TEMPORAL_ALPHA : 1.0f},
                           .grid = {H.grid[0][0], H.grid[0][1], 0, 0}};
        memcpy(tp.ndc_to_view, in->ndc_to_view, sizeof tp.ndc_to_view);
        memcpy(tp.prev_n2v, R.prev_n2v, sizeof tp.prev_n2v);
        memcpy(tp.m, m, sizeof tp.m);
        VkDescriptorImageInfo g_in = {in->nearest, R.iv[2], VK_IMAGE_LAYOUT_GENERAL},
                              h_out = {VK_NULL_HANDLE, R.hiv[R.hcur ^ 1], VK_IMAGE_LAYOUT_GENERAL},
                              h_in = {in->nearest, R.hiv[R.hcur], VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet wt[3] = {write(0, S, &g_in), write(1, I, &h_out), write(2, S, &h_in)};
        vk_ao_dispatch(ex, 0, R.temporal, wt, 3, &tp, sizeof tp, ((uint32_t)H.grid[0][0] + 7) / 8,
                       ((uint32_t)H.grid[0][1] + 7) / 8);   // counted with the blur (ao step 3)
        R.hcur ^= 1, R.hok = 1, R.hstamp = R.presents;
        R.bounce = R.hiv[R.hcur];
    } else R.hok = 0;
    memcpy(R.prev_view, in->view, sizeof R.prev_view);
    memcpy(R.prev_n2v, in->ndc_to_view, sizeof R.prev_n2v);
    R.prev_focal = in->focal;
    return 1;
}

VkImageView vk_rc_bounce(float *spacing, float *cap, float *strength, float *fade0, float *fade1) {
    *spacing = (float)R.spacing, *cap = RC_CAP, *strength = RC_STRENGTH, *fade0 = RC_FADE0, *fade1 = RC_FADE1;
    return R.bounce;
}

void vk_rc_show(VkExec *ex, const RcInputs *in, VkImageView out) {
    const VkDescriptorType S = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, I = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    ViewPush v = {.size = {(float)in->w, (float)in->h, (float)R.spacing, 0}, .grid = {R.gw, R.gh, 0, 0}};
    VkDescriptorImageInfo z_in = {in->nearest, in->z, VK_IMAGE_LAYOUT_GENERAL},
                          v_out = {VK_NULL_HANDLE, out, VK_IMAGE_LAYOUT_GENERAL},
                          g_in = {in->nearest, R.iv[2], VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wv[3] = {write(0, S, &z_in), write(1, I, &v_out), write(2, S, &g_in)};
    vk_ao_dispatch(ex, 0, R.view, wv, 3, &v, sizeof v, (in->w + 7) / 8, (in->h + 7) / 8);   // counted with the apply
}

void vk_rc_frame(int wanted) {
    R.idle = wanted ? 0 : R.idle + 1;
    R.presents++;
    if (R.hretry) R.hretry--;
    if (R.retry) R.retry--;
    // not at once: a click through the menu's choices would make and free the images per click
    if (R.idle == RC_OFF_PRESENTS && R.img[0]) {
        free_images(1);
        rt_log("vk: indirect light: images given back after %u presents without the gi level", RC_OFF_PRESENTS);
    }
}

void vk_rc_release(void) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    free_images(0);
    if (R.trace) vkDestroyPipeline(c->device, R.trace, NULL);
    if (R.resolve) vkDestroyPipeline(c->device, R.resolve, NULL);
    if (R.view) vkDestroyPipeline(c->device, R.view, NULL);
    if (R.temporal) vkDestroyPipeline(c->device, R.temporal, NULL);
    memset(&R, 0, sizeof R);
}
