// Radiance cascades over the depth buffer (vk_rc.c): the light the visible surfaces send each other, for the ambient
// occlusion's gi level (vk_ao.c, ao = gi). vk_ao.c run calls it on the occlusion's own images and pipeline layout.
#pragma once
#include "vk.h"
#include "vk_exec.h"   // VkExec

// What vk_ao.c lends: a compute pipeline on a layout (lit: the specialization constant 0, which a shader that does not
// declare it ignores), and one compute step through its six-binding layout (n > 0: an AC_GPU_PASSES row "ao step n"
// after it; 0 counts with the next).
VkPipeline vk_ao_comp_pipeline(const uint32_t *code, size_t size, VkPipelineLayout lay, int lit);
void vk_ao_dispatch(VkExec *ex, int n, VkPipeline pipe, const VkWriteDescriptorSet *w, uint32_t nw, const void *pc,
                    uint32_t pc_size, uint32_t gx, uint32_t gy);

// Which light of the samples counts (AC_RC_LIGHT): what they give beyond the sky they hide (above, the default), or
// all of it (zero).
enum { RC_LIGHT_ABOVE, RC_LIGHT_ZERO };

// The occlusion's inputs to the cascades, all in VK_IMAGE_LAYOUT_GENERAL.
typedef struct {
    VkPipelineLayout lay;                   // vk_ao.c's six-binding compute layout, 128 bytes of constants
    VkSampler nearest, linear, trilinear;   // vk_ao.c's samplers
    VkImageView z;                          // view z per occlusion texel (ao_prep.comp)
    VkImageView src;                        // the light sent out with its levels (ao_src.comp)
    VkImageView amb;                        // one coarse level of the game's ambient cube map
    uint32_t w, h;                          // the occlusion's size
    uint32_t src_levels;
    float ndc_to_view[4];                   // view x = z (ndc.x a + b), view y = z (ndc.y c + d), as ao.comp's
    float aspect;                           // how many times wider than tall a texel is in view space
    float amb_factor;                       // ambient light = amb_factor x the cube map along the normal
    float world[3][4];                      // .xyz: the rows of the view-to-world rotation
    int light;                              // RC_LIGHT_*
    int temporal;                           // AC_RC_TEMPORAL: g blended with last frame's (vk_rc.c)
    float view[3][4];                       // world to view (rows 0-2 of the camera block), for the history
    float focal;                            // the projection's y scale, for a camera cut
} RcInputs;

// Debug views (vk_ao.c read_settings, AC_RC_DEBUG with no settings row).
enum { RC_VIEW_NONE, RC_VIEW_G, RC_VIEW_OPEN };

// The cascades of this frame on in, recorded into ex's command buffer before the occlusion's blur, into g's image.
// view RC_VIEW_OPEN: the open share in place of g; with a view, cascade >= 0 traces that cascade alone, without those
// above it. 1 when they ran.
int vk_rc_run(VkExec *ex, const RcInputs *in, int view, int cascade);
// After a run: g's image (.rgb g, .a probe view z) and what ao_blur.comp needs to apply it.
VkImageView vk_rc_bounce(float *spacing, float *cap, float *strength, float *fade0, float *fade1);
// After a run with a view and the occlusion's blur: the view into out (vk_ao.c's factor image), which its apply then
// shows with the blend off.
void vk_rc_show(VkExec *ex, const RcInputs *in, VkImageView out);
// At each present (vk_ao_frame), with whether the gi level is chosen: after RC_OFF_PRESENTS presents without it the
// cascades' images are given back.
void vk_rc_frame(int wanted);
void vk_rc_release(void);   // vk_ao_release, the device idle: images and pipelines at once
