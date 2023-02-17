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

#include "vulkan_filter.h"

static int vulkan_filter_set_device(AVFilterContext *avctx,
                                    AVBufferRef *device)
{
    FFVulkanContext *s = avctx->priv;

    av_buffer_unref(&s->device_ref);

    s->device_ref = av_buffer_ref(device);
    if (!s->device_ref)
        return AVERROR(ENOMEM);

    s->device = (AVHWDeviceContext*)s->device_ref->data;
    s->hwctx  = s->device->hwctx;

    return 0;
}

static int vulkan_filter_set_frames(AVFilterContext *avctx,
                                    AVBufferRef *frames)
{
    FFVulkanContext *s = avctx->priv;

    av_buffer_unref(&s->frames_ref);

    s->frames_ref = av_buffer_ref(frames);
    if (!s->frames_ref)
        return AVERROR(ENOMEM);

    return 0;
}

int ff_vk_filter_config_input(AVFilterLink *inlink)
{
    int err;
    AVFilterContext *avctx = inlink->dst;
    FFVulkanContext *s = avctx->priv;
    AVHWFramesContext *input_frames;
    AVHWDeviceContext *dev;
    AVVulkanDeviceContext *vk_dev;

    if (!inlink->hw_frames_ctx) {
        av_log(avctx, AV_LOG_ERROR, "Vulkan filtering requires a "
               "hardware frames context on the input.\n");
        return AVERROR(EINVAL);
    }

    /* Extract the device and default output format from the first input. */
    if (avctx->inputs[0] != inlink)
        return 0;

    input_frames = (AVHWFramesContext *)inlink->hw_frames_ctx->data;
    if (input_frames->format != AV_PIX_FMT_VULKAN)
        return AVERROR(EINVAL);

    dev = (AVHWDeviceContext *)input_frames->device_ref->data;
    vk_dev = dev->hwctx;

    s->extensions = ff_vk_extensions_to_mask(vk_dev->enabled_dev_extensions,
                                             vk_dev->nb_enabled_dev_extensions);

    /**
     * libplacebo does not use descriptor buffers.
     */
    if (!(s->extensions & FF_VK_EXT_DESCRIPTOR_BUFFER) &&
        strcmp(avctx->filter->name, "libplacebo")) {
        av_log(avctx, AV_LOG_ERROR, "Vulkan filtering requires that "
               "the %s extension is supported 0x%x!\n",
               VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
               s->extensions & FF_VK_EXT_DESCRIPTOR_BUFFER);
        return AVERROR(EINVAL);
    }

    err = vulkan_filter_set_device(avctx, input_frames->device_ref);
    if (err < 0)
        return err;
    err = vulkan_filter_set_frames(avctx, inlink->hw_frames_ctx);
    if (err < 0)
        return err;

    err = ff_vk_load_functions(s->device, &s->vkfn, s->extensions, 1, 1);
    if (err < 0)
        return err;

    ff_vk_load_props(s);

    /* Default output parameters match input parameters. */
    s->input_format = input_frames->sw_format;
    if (s->output_format == AV_PIX_FMT_NONE)
        s->output_format = input_frames->sw_format;
    if (!s->output_width)
        s->output_width  = inlink->w;
    if (!s->output_height)
        s->output_height = inlink->h;

    return 0;
}

int ff_vk_filter_config_output_inplace(AVFilterLink *outlink)
{
    int err;
    AVFilterContext *avctx = outlink->src;
    FFVulkanContext *s = avctx->priv;

    av_buffer_unref(&outlink->hw_frames_ctx);

    if (!s->device_ref) {
        if (!avctx->hw_device_ctx) {
            av_log(avctx, AV_LOG_ERROR, "Vulkan filtering requires a "
                   "Vulkan device.\n");
            return AVERROR(EINVAL);
        }

        err = vulkan_filter_set_device(avctx, avctx->hw_device_ctx);
        if (err < 0)
            return err;
    }

    outlink->hw_frames_ctx = av_buffer_ref(s->frames_ref);
    if (!outlink->hw_frames_ctx)
        return AVERROR(ENOMEM);

    outlink->w = s->output_width;
    outlink->h = s->output_height;

    return 0;
}

int ff_vk_filter_config_output(AVFilterLink *outlink)
{
    int err;
    AVFilterContext *avctx = outlink->src;
    FFVulkanContext *s = avctx->priv;
    AVBufferRef *output_frames_ref;
    AVHWFramesContext *output_frames;

    av_buffer_unref(&outlink->hw_frames_ctx);

    if (!s->device_ref) {
        if (!avctx->hw_device_ctx) {
            av_log(avctx, AV_LOG_ERROR, "Vulkan filtering requires a "
                   "Vulkan device.\n");
            return AVERROR(EINVAL);
        }

        err = vulkan_filter_set_device(avctx, avctx->hw_device_ctx);
        if (err < 0)
            return err;
    }

    output_frames_ref = av_hwframe_ctx_alloc(s->device_ref);
    if (!output_frames_ref) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    output_frames = (AVHWFramesContext*)output_frames_ref->data;

    output_frames->format    = AV_PIX_FMT_VULKAN;
    output_frames->sw_format = s->output_format;
    output_frames->width     = s->output_width;
    output_frames->height    = s->output_height;

    err = av_hwframe_ctx_init(output_frames_ref);
    if (err < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to initialise output "
               "frames: %d.\n", err);
        goto fail;
    }

    outlink->hw_frames_ctx = output_frames_ref;
    outlink->w = s->output_width;
    outlink->h = s->output_height;

    return 0;
fail:
    av_buffer_unref(&output_frames_ref);
    return err;
}

int ff_vk_filter_init(AVFilterContext *avctx)
{
    FFVulkanContext *s = avctx->priv;

    s->output_format = AV_PIX_FMT_NONE;

    return 0;
}

int ff_vk_filter_process_simple(FFVulkanContext *vkctx, FFVkExecPool *e,
                                FFVulkanPipeline *pl, AVFrame *out_f, AVFrame *in_f,
                                VkSampler sampler, void *push_src, size_t push_size)
{
    int err = 0;
    FFVulkanFunctions *vk = &vkctx->vkfn;
    VkImageView in_views[AV_NUM_DATA_POINTERS];
    VkImageView out_views[AV_NUM_DATA_POINTERS];
    VkImageMemoryBarrier2 img_bar[37];
    int nb_img_bar = 0;

    /* Update descriptors and init the exec context */
    FFVkExecContext *exec = ff_vk_exec_get(e);
    ff_vk_exec_start(vkctx, exec);

    ff_vk_exec_bind_pipeline(vkctx, exec, pl);

    if (push_src)
        ff_vk_update_push_exec(vkctx, exec, pl, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, push_size, push_src);

    RET(ff_vk_exec_add_dep_frame(vkctx, exec, in_f,
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT));
    RET(ff_vk_exec_add_dep_frame(vkctx, exec, out_f,
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT));

    RET(ff_vk_create_imageviews(vkctx, exec, in_views,  in_f));
    RET(ff_vk_create_imageviews(vkctx, exec, out_views, out_f));

    ff_vk_update_descriptor_img_array(vkctx, pl, exec,  in_f,  in_views, 0, 0,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                      sampler);
    ff_vk_update_descriptor_img_array(vkctx, pl, exec, out_f, out_views, 0, 1,
                                      VK_IMAGE_LAYOUT_GENERAL,
                                      NULL);

    ff_vk_frame_barrier(vkctx, exec, in_f, img_bar, &nb_img_bar,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_QUEUE_FAMILY_IGNORED);
    ff_vk_frame_barrier(vkctx, exec, out_f, img_bar, &nb_img_bar,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_WRITE_BIT,
                        VK_IMAGE_LAYOUT_GENERAL,
                        VK_QUEUE_FAMILY_IGNORED);

    vk->CmdPipelineBarrier2KHR(exec->buf, &(VkDependencyInfo) {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
            .pImageMemoryBarriers = img_bar,
            .imageMemoryBarrierCount = nb_img_bar,
        });

    vk->CmdDispatch(exec->buf,
                    FFALIGN(vkctx->output_width,  pl->wg_size[0])/pl->wg_size[0],
                    FFALIGN(vkctx->output_height, pl->wg_size[1])/pl->wg_size[1],
                    pl->wg_size[1]);

    return ff_vk_exec_submit(vkctx, exec);
fail:
    ff_vk_exec_discard_deps(vkctx, exec);
    return err;
}

int ff_vk_filter_process_2pass(FFVulkanContext *vkctx, FFVkExecPool *e,
                               FFVulkanPipeline *pls[2],
                               AVFrame *out, AVFrame *tmp, AVFrame *in,
                               VkSampler sampler, void *push_src, size_t push_size)
{
    int err = 0;
    FFVulkanFunctions *vk = &vkctx->vkfn;
    VkImageView in_views[AV_NUM_DATA_POINTERS];
    VkImageView tmp_views[AV_NUM_DATA_POINTERS];
    VkImageView out_views[AV_NUM_DATA_POINTERS];
    VkImageMemoryBarrier2 img_bar[37];
    int nb_img_bar = 0;

    /* Update descriptors and init the exec context */
    FFVkExecContext *exec = ff_vk_exec_get(e);
    ff_vk_exec_start(vkctx, exec);

    RET(ff_vk_exec_add_dep_frame(vkctx, exec, in,
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT));
    RET(ff_vk_exec_add_dep_frame(vkctx, exec, tmp,
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT));
    RET(ff_vk_exec_add_dep_frame(vkctx, exec, out,
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT));

    RET(ff_vk_create_imageviews(vkctx, exec, in_views,  in));
    RET(ff_vk_create_imageviews(vkctx, exec, tmp_views, tmp));
    RET(ff_vk_create_imageviews(vkctx, exec, out_views, out));

    ff_vk_frame_barrier(vkctx, exec, in, img_bar, &nb_img_bar,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_QUEUE_FAMILY_IGNORED);
    ff_vk_frame_barrier(vkctx, exec, tmp, img_bar, &nb_img_bar,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                        VK_IMAGE_LAYOUT_GENERAL,
                        VK_QUEUE_FAMILY_IGNORED);
    ff_vk_frame_barrier(vkctx, exec, out, img_bar, &nb_img_bar,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_WRITE_BIT,
                        VK_IMAGE_LAYOUT_GENERAL,
                        VK_QUEUE_FAMILY_IGNORED);

    vk->CmdPipelineBarrier2KHR(exec->buf, &(VkDependencyInfo) {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
            .pImageMemoryBarriers = img_bar,
            .imageMemoryBarrierCount = nb_img_bar,
        });

    for (int i = 0; i < 2; i++) {
        FFVulkanPipeline *pl = pls[i];
        AVFrame *src_f = !i ? in : tmp;
        AVFrame *dst_f = !i ? tmp : out;
        VkImageView *src_views = !i ? in_views : tmp_views;
        VkImageView *dst_views = !i ? tmp_views : out_views;

        ff_vk_exec_bind_pipeline(vkctx, exec, pl);

        if (push_src)
            ff_vk_update_push_exec(vkctx, exec, pl, VK_SHADER_STAGE_COMPUTE_BIT,
                                   0, push_size, push_src);

        ff_vk_update_descriptor_img_array(vkctx, pl, exec, src_f, src_views, 0, 0,
                                          !i ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL :
                                               VK_IMAGE_LAYOUT_GENERAL,
                                          sampler);
        ff_vk_update_descriptor_img_array(vkctx, pl, exec, dst_f, dst_views, 0, 1,
                                          VK_IMAGE_LAYOUT_GENERAL,
                                          NULL);

        vk->CmdDispatch(exec->buf,
                        FFALIGN(vkctx->output_width,  pl->wg_size[0])/pl->wg_size[0],
                        FFALIGN(vkctx->output_height, pl->wg_size[1])/pl->wg_size[1],
                        pl->wg_size[1]);
    }

    return ff_vk_exec_submit(vkctx, exec);
fail:
    ff_vk_exec_discard_deps(vkctx, exec);
    return err;
}

int ff_vk_filter_process_2in(FFVulkanContext *vkctx, FFVkExecPool *e,
                             FFVulkanPipeline *pl,
                             AVFrame *out, AVFrame *in1, AVFrame *in2,
                             VkSampler sampler, void *push_src, size_t push_size)
{
    int err = 0;
    FFVulkanFunctions *vk = &vkctx->vkfn;
    VkImageView in1_views[AV_NUM_DATA_POINTERS];
    VkImageView in2_views[AV_NUM_DATA_POINTERS];
    VkImageView out_views[AV_NUM_DATA_POINTERS];
    VkImageMemoryBarrier2 img_bar[37];
    int nb_img_bar = 0;

    /* Update descriptors and init the exec context */
    FFVkExecContext *exec = ff_vk_exec_get(e);
    ff_vk_exec_start(vkctx, exec);

    RET(ff_vk_exec_add_dep_frame(vkctx, exec, in1,
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT));
    RET(ff_vk_exec_add_dep_frame(vkctx, exec, in2,
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT));
    RET(ff_vk_exec_add_dep_frame(vkctx, exec, out,
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT));

    RET(ff_vk_create_imageviews(vkctx, exec, in1_views, in1));
    RET(ff_vk_create_imageviews(vkctx, exec, in2_views, in2));
    RET(ff_vk_create_imageviews(vkctx, exec, out_views, out));

    ff_vk_frame_barrier(vkctx, exec, in1, img_bar, &nb_img_bar,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_QUEUE_FAMILY_IGNORED);
    ff_vk_frame_barrier(vkctx, exec, in2, img_bar, &nb_img_bar,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_QUEUE_FAMILY_IGNORED);
    ff_vk_frame_barrier(vkctx, exec, out, img_bar, &nb_img_bar,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_WRITE_BIT,
                        VK_IMAGE_LAYOUT_GENERAL,
                        VK_QUEUE_FAMILY_IGNORED);

    vk->CmdPipelineBarrier2KHR(exec->buf, &(VkDependencyInfo) {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
            .pImageMemoryBarriers = img_bar,
            .imageMemoryBarrierCount = nb_img_bar,
        });

    ff_vk_exec_bind_pipeline(vkctx, exec, pl);

    if (push_src)
        ff_vk_update_push_exec(vkctx, exec, pl, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, push_size, push_src);

    ff_vk_update_descriptor_img_array(vkctx, pl, exec, in1, in1_views, 0, 0,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                      sampler);
    ff_vk_update_descriptor_img_array(vkctx, pl, exec, in2, in2_views, 0, 1,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                      sampler);
    ff_vk_update_descriptor_img_array(vkctx, pl, exec, out, out_views, 0, 2,
                                      VK_IMAGE_LAYOUT_GENERAL,
                                      NULL);

    vk->CmdDispatch(exec->buf,
                    FFALIGN(vkctx->output_width,  pl->wg_size[0])/pl->wg_size[0],
                    FFALIGN(vkctx->output_height, pl->wg_size[1])/pl->wg_size[1],
                    pl->wg_size[1]);

    return ff_vk_exec_submit(vkctx, exec);
fail:
    ff_vk_exec_discard_deps(vkctx, exec);
    return err;
}
