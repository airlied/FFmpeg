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
    AVBufferPool *buf_pool;

    FFVulkanPipeline pl_int_hor;
    FFVkSPIRVShader shd_int_hor;

    FFVulkanPipeline pl_weights;
    FFVkSPIRVShader shd_weights;

    FFVulkanPipeline pl_denoise;
    FFVkSPIRVShader shd_denoise;

    double sigma;
    int patch_size;
    int patch_size_uv;
    int research_size;
    int research_size_uv;
} NLMeansVulkanContext;

typedef struct IntegralPushData {
    unsigned int int_stride[4];
} IntegralPushData;

static av_cold int init_hor_pipeline(FFVulkanContext *vkctx, FFVkExecPool *exec,
                                     FFVulkanPipeline *pl, FFVkSPIRVShader *shd,
                                     VkSampler sampler, int planes,
                                     FFVkSPIRVCompiler *spv)
{
    int err;
    uint8_t *spv_data;
    size_t spv_len;
    void *spv_opaque;
    FFVulkanDescriptorSetBinding *desc;

    RET(ff_vk_shader_init(pl, shd, "nlmeans_integral_hor",
                          VK_SHADER_STAGE_COMPUTE_BIT, 0));

    ff_vk_shader_set_compute_sizes(shd, 32, 32, 1);

    GLSLC(0, layout(push_constant, std430) uniform pushConstants {        );
    GLSLC(1,    uvec4 int_stride;                                         );
    GLSLC(0, };                                                           );

    ff_vk_add_push_constant(pl, 0, sizeof(IntegralPushData),
                            VK_SHADER_STAGE_COMPUTE_BIT);

    desc = (FFVulkanDescriptorSetBinding []) {
        {
            .name       = "input_img",
            .type       = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .dimensions = 2,
            .elems      = planes,
            .stages     = VK_SHADER_STAGE_COMPUTE_BIT,
            .samplers   = DUP_SAMPLER(sampler),
        },
    };
    RET(ff_vk_pipeline_descriptor_set_add(vkctx, pl, shd, desc, 1, 0, 0));

    GLSLC(0, layout(buffer_reference, buffer_reference_align = 64) buffer IntegralRows { );
    GLSLC(1,     mat4 sum;                                                                       );
    GLSLC(0, };                                                                                  );

    desc = (FFVulkanDescriptorSetBinding []) {
        {
            .name        = "integral_rows",
            .type        = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .mem_layout  = "std430",
            .stages      = VK_SHADER_STAGE_COMPUTE_BIT,
            .buf_content = "IntegralRows integral_data[4];",
        },
    };

    RET(ff_vk_pipeline_descriptor_set_add(vkctx, pl, shd, desc, 1, 0, 0));

    GLSLC(0, void main()                                                  );
    GLSLC(0, {                                                            );
    GLSLC(1,     ivec2 size;                                              );
    GLSLC(1,     const ivec2 pos = ivec2(gl_GlobalInvocationID.xy);       );
    GLSLC(0, }                                                            );

    RET(spv->compile_shader(spv, vkctx, shd, &spv_data, &spv_len, "main",
                            &spv_opaque));
    RET(ff_vk_shader_create(vkctx, shd, spv_data, spv_len, "main"));

    RET(ff_vk_init_compute_pipeline(vkctx, pl, shd));
    RET(ff_vk_exec_pipeline_register(vkctx, exec, pl));

    return 0;

fail:
    if (spv_opaque)
        spv->free_shader(spv, &spv_opaque);

    return err;
}

typedef struct WeightsPushData {
    unsigned int int_stride[4];
    unsigned int buf_stride[4];
    unsigned int patch_size[4];
    float sigma[4];
} WeightsPushData;

static av_cold int init_weights_pipeline(FFVulkanContext *vkctx, FFVkExecPool *exec,
                                         FFVulkanPipeline *pl, FFVkSPIRVShader *shd,
                                         VkSampler sampler, int planes,
                                         FFVkSPIRVCompiler *spv)
{
    int err;
    uint8_t *spv_data;
    size_t spv_len;
    void *spv_opaque;
    FFVulkanDescriptorSetBinding *desc;

    RET(ff_vk_shader_init(pl, shd, "nlmeans_weights",
                          VK_SHADER_STAGE_COMPUTE_BIT, 0));

    ff_vk_shader_set_compute_sizes(shd, 32, 32, 1);

    GLSLC(0, layout(push_constant, std430) uniform pushConstants {        );
    GLSLC(1,    uvec4 int_stride;                                         );
    GLSLC(1,    uvec4 buffer_stride;                                      );
    GLSLC(1,    uvec4 patch_size;                                         );
    GLSLC(1,    vec4 sigma;                                               );
    GLSLC(0, };                                                           );

    ff_vk_add_push_constant(pl, 0, sizeof(WeightsPushData),
                            VK_SHADER_STAGE_COMPUTE_BIT);

    desc = (FFVulkanDescriptorSetBinding []) {
        {
            .name       = "input_img",
            .type       = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .dimensions = 2,
            .elems      = planes,
            .stages     = VK_SHADER_STAGE_COMPUTE_BIT,
            .samplers   = DUP_SAMPLER(sampler),
        },
    };
    RET(ff_vk_pipeline_descriptor_set_add(vkctx, pl, shd, desc, 1, 0, 0));

    GLSLC(0, layout(buffer_reference, buffer_reference_align = 16) readonly buffer IntegralRows { );
    GLSLC(1,     mat4 sum;                                                                                );
    GLSLC(0, };                                                                                           );
    GLSLC(0, layout(buffer_reference, buffer_reference_align = 16) writeonly buffer WeightData {  );
    GLSLC(1,     vec4 weight;                                                                             );
    GLSLC(1,     vec4 sum;                                                                                );
    GLSLC(0, };                                                                                           );

    desc = (FFVulkanDescriptorSetBinding []) {
        {
            .name        = "integral_rows",
            .type        = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .mem_layout  = "std430",
            .stages      = VK_SHADER_STAGE_COMPUTE_BIT,
            .buf_content = "IntegralRows integral_data[4];",
        },
        {
            .name        = "weights_data",
            .type        = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .mem_quali   = "writeonly",
            .mem_layout  = "std430",
            .stages      = VK_SHADER_STAGE_COMPUTE_BIT,
            .buf_content = "WeightData weights[4];",
        },
    };

    RET(ff_vk_pipeline_descriptor_set_add(vkctx, pl, shd, desc, 2, 0, 0));


    GLSLC(0, void main()                                                  );
    GLSLC(0, {                                                            );
    GLSLC(1,     ivec2 size;                                              );
    GLSLC(1,     const ivec2 pos = ivec2(gl_GlobalInvocationID.xy);       );
    GLSLC(0, }                                                            );

    RET(spv->compile_shader(spv, vkctx, shd, &spv_data, &spv_len, "main",
                            &spv_opaque));
    RET(ff_vk_shader_create(vkctx, shd, spv_data, spv_len, "main"));

    RET(ff_vk_init_compute_pipeline(vkctx, pl, shd));
    RET(ff_vk_exec_pipeline_register(vkctx, exec, pl));

    return 0;

fail:
    if (spv_opaque)
        spv->free_shader(spv, &spv_opaque);

    return err;
}

static av_cold int init_denoise_pipeline(FFVulkanContext *vkctx, FFVkExecPool *exec,
                                         FFVulkanPipeline *pl, FFVkSPIRVShader *shd,
                                         VkSampler sampler, int planes,
                                         FFVkSPIRVCompiler *spv)
{
    int err;
    uint8_t *spv_data;
    size_t spv_len;
    void *spv_opaque;
    FFVulkanDescriptorSetBinding *desc;

    RET(ff_vk_shader_init(pl, shd, "nlmeans_denoise",
                          VK_SHADER_STAGE_COMPUTE_BIT, 0));

    ff_vk_shader_set_compute_sizes(shd, 32, 32, 1);

    GLSLC(0, layout(push_constant, std430) uniform pushConstants {        );
    GLSLC(1,    uvec4 int_stride;                                         );
    GLSLC(1,    uvec4 buffer_stride;                                      );
    GLSLC(1,    uvec4 patch_size;                                         );
    GLSLC(1,    vec4 sigma;                                               );
    GLSLC(0, };                                                           );

    ff_vk_add_push_constant(pl, 0, sizeof(WeightsPushData),
                            VK_SHADER_STAGE_COMPUTE_BIT);

    desc = (FFVulkanDescriptorSetBinding []) {
        {
            .name       = "input_img",
            .type       = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .dimensions = 2,
            .elems      = planes,
            .stages     = VK_SHADER_STAGE_COMPUTE_BIT,
            .samplers   = DUP_SAMPLER(sampler),
        },
        {
            .name       = "output_img",
            .type       = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .mem_layout = ff_vk_shader_rep_fmt(vkctx->output_format),
            .mem_quali  = "writeonly",
            .dimensions = 2,
            .elems      = planes,
            .stages     = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    };
    RET(ff_vk_pipeline_descriptor_set_add(vkctx, pl, shd, desc, 2, 0, 0));

    GLSLC(0, layout(buffer_reference, buffer_reference_align = 32) readonly buffer WeightData { );
    GLSLC(1,     vec4 weight;                                                                           );
    GLSLC(1,     vec4 sum;                                                                              );
    GLSLC(0, };                                                                                         );

    desc = (FFVulkanDescriptorSetBinding []) {
        {
            .name        = "weights_data",
            .type        = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .mem_quali   = "readonly",
            .mem_layout  = "std430",
            .stages      = VK_SHADER_STAGE_COMPUTE_BIT,
            .buf_content = "WeightData weights[4];",
        },
    };

    RET(ff_vk_pipeline_descriptor_set_add(vkctx, pl, shd, desc, 1, 0, 0));

    GLSLC(0, void main()                                                                        );
    GLSLC(0, {                                                                                  );
    GLSLC(1,     ivec2 size;                                                                    );
    GLSLC(1,     const ivec2 pos = ivec2(gl_GlobalInvocationID.xy);                             );
    GLSLF(1,  size = imageSize(output_img[%i]);                                               ,0);
    GLSLC(1,  if (IS_WITHIN(pos, size)) {                                                       );
    GLSLF(2,      vec4 weight = weights[%i][pos.y * buffer_stride[%i] + pos.x].weight;     ,0, 0);
    GLSLC(1,  }                                                                                 );
    GLSLC(0, }                                                                                  );

    RET(spv->compile_shader(spv, vkctx, shd, &spv_data, &spv_len, "main",
                            &spv_opaque));
    RET(ff_vk_shader_create(vkctx, shd, spv_data, spv_len, "main"));

    RET(ff_vk_init_compute_pipeline(vkctx, pl, shd));
    RET(ff_vk_exec_pipeline_register(vkctx, exec, pl));

    return 0;

fail:
    if (spv_opaque)
        spv->free_shader(spv, &spv_opaque);

    return err;
}


static av_cold int init_filter(AVFilterContext *ctx)
{
    int err;
    NLMeansVulkanContext *s = ctx->priv;
    FFVulkanContext *vkctx = &s->vkctx;
    const int planes = av_pix_fmt_count_planes(s->vkctx.output_format);
    FFVkSPIRVCompiler *spv;

    spv = ff_vk_spirv_init();
    if (!spv) {
        av_log(ctx, AV_LOG_ERROR, "Unable to initialize SPIR-V compiler!\n");
        return AVERROR_EXTERNAL;
    }

    ff_vk_qf_init(vkctx, &s->qf, VK_QUEUE_COMPUTE_BIT);
    RET(ff_vk_exec_pool_init(vkctx, &s->qf, &s->e, s->qf.nb_queues*4, 0, 0, 0, NULL));
    RET(ff_vk_init_sampler(vkctx, &s->sampler, 1, VK_FILTER_LINEAR));

    RET(init_hor_pipeline(vkctx, &s->e, &s->pl_int_hor, &s->shd_int_hor, s->sampler,
                          planes, spv));

    RET(init_weights_pipeline(vkctx, &s->e, &s->pl_weights, &s->shd_weights, s->sampler,
                              planes, spv));

    RET(init_denoise_pipeline(vkctx, &s->e, &s->pl_denoise, &s->shd_denoise, s->sampler,
                              planes, spv));

    s->initialized = 1;

    return 0;

fail:
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
    AVBufferRef *weights_buf;
    AVBufferRef *integral_buf;
    size_t buf_size;

    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    if (!s->initialized)
        RET(init_filter(ctx));











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
    ff_vk_pipeline_free(vkctx, &s->pl_int_hor);
    ff_vk_shader_free(vkctx, &s->shd_int_hor);
    ff_vk_pipeline_free(vkctx, &s->pl_weights);
    ff_vk_shader_free(vkctx, &s->shd_weights);
    ff_vk_pipeline_free(vkctx, &s->pl_denoise);
    ff_vk_shader_free(vkctx, &s->shd_denoise);

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
