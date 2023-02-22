/*
 * Copyright (c) Lynne
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/random_seed.h"
#include "libavutil/opt.h"
#include "vulkan_filter.h"
#include "vulkan_spirv.h"
#include "internal.h"

typedef struct NLMeansVulkanContext {
    FFVulkanContext vkctx;

    int initialized;
    FFVkExecPool e;
    FFVkQueueFamilyCtx qf;
    VkSampler sampler;
    FFVulkanPipeline pl;
    FFVkSPIRVShader shd;

    /* Push constants / options */
    struct {
        int32_t filter_len[2];
        float filter_norm[4];
    } opts;

    double sigma;
    int patch_size;
    int patch_size_uv;
    int research_size;
    int research_size_uv;
} NLMeansVulkanContext;

static const char nlmeans_kernel[] = {
    C(0, vec4 sum(const ivec2 pos, const int idx)                                 )
    C(0, {                                                                        )
    C(1,     float p0   = texture(input_image[idx], pos);                         )
    C(1,     vec4  p1.x = texture(input_image[idx], pos + ivec2(dx.x, dy.x)).x;   )
    C(1,           p1.y = texture(input_image[idx], pos + ivec2(dx.y, dy.y)).x;   )
    C(1,           p1.z = texture(input_image[idx], pos + ivec2(dx.z, dy.z)).x;   )
    C(1,           p1.w = texture(input_image[idx], pos + ivec2(dx.w, dy.w)).x;   )
    C(1,     sum += (p0 - p1) * (p0 - p1) * 255 * 255;                            )
    C(0, }                                                                        )
};

static av_cold int init_filter(AVFilterContext *ctx, AVFrame *in)
{
    int err;
    uint8_t *spv_data;
    size_t spv_len;
    void *spv_opaque;
    NLMeansVulkanContext *s = ctx->priv;
    FFVulkanContext *vkctx = &s->vkctx;
    const int planes = av_pix_fmt_count_planes(s->vkctx.output_format);
    FFVkSPIRVShader *shd;
    FFVkSPIRVCompiler *spv;
    FFVulkanDescriptorSetBinding *desc;

    spv = ff_vk_spirv_init();
    if (!spv) {
        av_log(ctx, AV_LOG_ERROR, "Unable to initialize SPIR-V compiler!\n");
        return AVERROR_EXTERNAL;
    }

    ff_vk_qf_init(vkctx, &s->qf, VK_QUEUE_COMPUTE_BIT);
    RET(ff_vk_exec_pool_init(vkctx, &s->qf, &s->e, s->qf.nb_queues*4, 0, 0, 0, NULL));
    RET(ff_vk_init_sampler(vkctx, &s->sampler, 1, VK_FILTER_LINEAR));
    RET(ff_vk_shader_init(&s->pl, &s->shd, "nlmeans_integrate",
                          VK_SHADER_STAGE_COMPUTE_BIT));
    shd = &s->shd;

    ff_vk_shader_set_compute_sizes(shd, 32, 32, 1);

    desc = (FFVulkanDescriptorSetBinding []) {
        {
            .name       = "input_img",
            .type       = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .dimensions = 2,
            .elems      = planes,
            .stages     = VK_SHADER_STAGE_COMPUTE_BIT,
            .samplers   = DUP_SAMPLER(s->sampler),
        },
        {
            .name       = "output_img",
            .type       = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .mem_layout = ff_vk_shader_rep_fmt(s->vkctx.output_format),
            .mem_quali  = "writeonly",
            .dimensions = 2,
            .elems      = planes,
            .stages     = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    };

    RET(ff_vk_pipeline_descriptor_set_add(vkctx, &s->pl, shd, desc, 2, 0, 0));

    GLSLC(0, layout(push_constant, std430) uniform pushConstants {        );
    GLSLC(1,    ivec2 filter_len;                                         );
    GLSLC(1,    vec4 filter_norm;                                         );
    GLSLC(0, };                                                           );
    GLSLC(0,                                                              );

    ff_vk_add_push_constant(&s->pl, 0, sizeof(s->opts),
                            VK_SHADER_STAGE_COMPUTE_BIT);

    GLSLD(   nlmeans_kernel                                               );
    GLSLC(0, void main()                                                  );
    GLSLC(0, {                                                            );
    GLSLC(1,     ivec2 size;                                              );
    GLSLC(1,     const ivec2 pos = ivec2(gl_GlobalInvocationID.xy);       );
    for (int i = 0; i < planes; i++) {
        GLSLC(0,                                                          );
        GLSLF(1,  size = imageSize(output_img[%i]);                     ,i);
        GLSLC(1,  if (IS_WITHIN(pos, size)) {                             );
        if (i > 0 && (s->patch_size_uv > 1)) {
            GLSLF(2, distort(pos, %i);                                  ,i);
        } else {
            GLSLF(2, vec4 res = texture(input_img[%i], pos);            ,i);
            GLSLF(2, imageStore(output_img[%i], pos, res);              ,i);
        }
        GLSLC(1, }                                                        );
    }
    GLSLC(0, }                                                            );

    RET(spv->compile_shader(spv, ctx, &s->shd, &spv_data, &spv_len, "main",
                            &spv_opaque));
    RET(ff_vk_shader_create(vkctx, &s->shd, spv_data, spv_len, "main"));

    RET(ff_vk_init_compute_pipeline(vkctx, &s->pl, &s->shd));
    RET(ff_vk_exec_pipeline_register(vkctx, &s->e, &s->pl));

    s->initialized = 1;

    return 0;

fail:
    if (spv_opaque)
        spv->free_shader(spv, &spv_opaque);
    if (spv)
        spv->uninit(&spv);

    return err;
}

static int nlmeans_vulkan_filter_frame(AVFilterLink *link, AVFrame *in)
{
    int err;
    AVFrame *out = NULL;
    AVFilterContext *ctx = link->dst;
    NLMeansVulkanContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];

    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    if (!s->initialized)
        RET(init_filter(ctx, in));

    RET(ff_vk_filter_process_simple(&s->vkctx, &s->e, &s->pl,
                                    out, in, s->sampler, &s->opts, sizeof(s->opts)));

    err = av_frame_copy_props(out, in);
    if (err < 0)
        goto fail;

    av_frame_free(&in);

    return ff_filter_frame(outlink, out);

fail:
    av_frame_free(&in);
    av_frame_free(&out);
    return err;
}

static void nlmeans_vulkan_uninit(AVFilterContext *avctx)
{
    NLMeansVulkanContext *s = avctx->priv;
    FFVulkanContext *vkctx = &s->vkctx;
    FFVulkanFunctions *vk = &vkctx->vkfn;

    ff_vk_exec_pool_free(vkctx, &s->e);
    ff_vk_pipeline_free(vkctx, &s->pl);
    ff_vk_shader_free(vkctx, &s->shd);

    if (s->sampler)
        vk->DestroySampler(vkctx->hwctx->act_dev, s->sampler,
                           vkctx->hwctx->alloc);

    ff_vk_uninit(&s->vkctx);

    s->initialized = 0;
}

#define OFFSET(x) offsetof(NLMeansVulkanContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption nlmeans_vulkan_options[] = {
    { "s",  "denoising strength",                OFFSET(sigma),            AV_OPT_TYPE_DOUBLE, { .dbl = 1.0   }, 1.0, 30.0, FLAGS },
    { "p",  "patch size",                        OFFSET(patch_size),       AV_OPT_TYPE_INT,    { .i64 = 2*3+1 },   0,   99, FLAGS },
    { "pc", "patch size for chroma planes",      OFFSET(patch_size_uv),    AV_OPT_TYPE_INT,    { .i64 = 0     },   0,   99, FLAGS },
    { "r",  "research window",                   OFFSET(research_size),    AV_OPT_TYPE_INT,    { .i64 = 7*2+1 },   0,   99, FLAGS },
    { "rc", "research window for chroma planes", OFFSET(research_size_uv), AV_OPT_TYPE_INT,    { .i64 = 0     },   0,   99, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(nlmeans_vulkan);

static const AVFilterPad nlmeans_vulkan_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = &nlmeans_vulkan_filter_frame,
        .config_props = &ff_vk_filter_config_input,
    },
};

static const AVFilterPad nlmeans_vulkan_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props = &ff_vk_filter_config_output,
    },
};

const AVFilter ff_vf_nlmeans_vulkan = {
    .name           = "nlmeans_vulkan",
    .description    = NULL_IF_CONFIG_SMALL("Non-local means denoiser (Vulkan)"),
    .priv_size      = sizeof(NLMeansVulkanContext),
    .init           = &ff_vk_filter_init,
    .uninit         = &nlmeans_vulkan_uninit,
    FILTER_INPUTS(nlmeans_vulkan_inputs),
    FILTER_OUTPUTS(nlmeans_vulkan_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_VULKAN),
    .priv_class     = &nlmeans_vulkan_class,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
