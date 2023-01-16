/*
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

#include "vulkan_video.h"
#include "vulkan_decode.h"
#include "config_components.h"

#if CONFIG_H264_VULKAN_HWACCEL
extern const VkExtensionProperties ff_vk_dec_h264_ext;
#endif
#if CONFIG_HEVC_VULKAN_HWACCEL
extern const VkExtensionProperties ff_vk_dec_hevc_ext;
#endif

static const VkExtensionProperties *dec_ext[] = {
#if CONFIG_H264_VULKAN_HWACCEL
    [AV_CODEC_ID_H264] = &ff_vk_dec_h264_ext,
#endif
#if CONFIG_HEVC_VULKAN_HWACCEL
    [AV_CODEC_ID_HEVC] = &ff_vk_dec_hevc_ext,
#endif
};

static int vk_decode_create_view(FFVulkanDecodeContext *ctx, VkImageView *dst_view,
                                 VkImageAspectFlags *aspect, AVVkFrame *src,
                                 VkFormat vkf)
{
    VkResult ret;
    FFVulkanFunctions *vk = &ctx->s.vkfn;
    VkImageAspectFlags aspect_mask = ff_vk_aspect_bits_from_vkfmt(vkf);

    VkSamplerYcbcrConversionInfo yuv_sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
        .conversion = ctx->yuv_sampler,
    };
    VkImageViewCreateInfo img_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = &yuv_sampler_info,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = vkf,
        .image = src->img[0],
        .components = (VkComponentMapping) {
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange = (VkImageSubresourceRange) {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseArrayLayer = 0,
            .layerCount     = VK_REMAINING_ARRAY_LAYERS,
            .levelCount     = 1,
        },
    };

    ret = vk->CreateImageView(ctx->s.hwctx->act_dev, &img_view_create_info,
                              ctx->s.hwctx->alloc, dst_view);
    if (ret != VK_SUCCESS)
        return AVERROR_EXTERNAL;

    *aspect = aspect_mask;

    return 0;
}

static AVFrame *vk_get_dpb_pool(FFVulkanDecodeContext *ctx)
{
    AVFrame *avf = av_frame_alloc();
    AVHWFramesContext *dpb_frames = (AVHWFramesContext *)ctx->dpb_hwfc_ref->data;
    if (!avf)
        return NULL;

    avf->hw_frames_ctx = av_buffer_ref(ctx->dpb_hwfc_ref);
    if (!avf->hw_frames_ctx)
        av_frame_free(&avf);
    avf->buf[0] = av_buffer_pool_get(dpb_frames->pool);
    if (!avf->buf[0])
        av_frame_free(&avf);
    avf->data[0] = avf->buf[0]->data;

    return avf;
}

int ff_vk_decode_prepare_frame(FFVulkanDecodeContext *ctx, AVFrame *pic,
                               FFVulkanDecodePicture *vkpic, int is_current,
                               int alloc_dpb)
{
    int err;

    vkpic->nb_slices = 0;
    vkpic->slices_size = 0;

    /* If the decoder made a blank frame to make up for a missing ref, or the
     * frame is the current frame so it's missing one, create a re-representation */
    if (vkpic->img_view_ref)
        return 0;

    /* Pre-allocate slice buffer with a reasonable default */
    if (is_current) {
        uint64_t min_alloc = 4096;
        if (0)
            min_alloc = 2*ctx->s.hprops.minImportedHostPointerAlignment;

        vkpic->slices = av_fast_realloc(NULL, &vkpic->slices_size_max, min_alloc);
        if (!vkpic->slices)
            return AVERROR(ENOMEM);

        if (0)
            vkpic->slices_size += ctx->s.hprops.minImportedHostPointerAlignment;
    }

    vkpic->dpb_frame    = NULL;
    vkpic->img_view_ref = NULL;
    vkpic->img_view_out = NULL;

    if (ctx->layered_dpb && alloc_dpb) {
        vkpic->img_view_ref = ctx->layered_view;
        vkpic->img_aspect_ref = ctx->layered_aspect;
    } else if (alloc_dpb) {
        AVHWFramesContext *dpb_frames = (AVHWFramesContext *)ctx->dpb_hwfc_ref->data;
        AVVulkanFramesContext *dpb_hwfc = dpb_frames->hwctx;

        vkpic->dpb_frame = vk_get_dpb_pool(ctx);
        if (!vkpic->dpb_frame)
            return AVERROR(ENOMEM);

        err = vk_decode_create_view(ctx, &vkpic->img_view_ref,
                                    &vkpic->img_aspect_ref,
                                    (AVVkFrame *)vkpic->dpb_frame->data[0],
                                    dpb_hwfc->format[0]);
        if (err < 0)
            return err;
    }

    if (!alloc_dpb || is_current) {
        AVHWFramesContext *frames = (AVHWFramesContext *)pic->hw_frames_ctx->data;
        AVVulkanFramesContext *hwfc = frames->hwctx;

        err = vk_decode_create_view(ctx, &vkpic->img_view_out,
                                    &vkpic->img_aspect,
                                    (AVVkFrame *)pic->buf[0]->data,
                                    hwfc->format[0]);
        if (err < 0)
            return err;

        if (!alloc_dpb) {
            vkpic->img_view_ref = vkpic->img_view_out;
            vkpic->img_aspect_ref = vkpic->img_aspect;
        }
    }

    return 0;
}

int ff_vk_decode_add_slice(FFVulkanDecodePicture *vp,
                           const uint8_t *data, size_t size, int add_startcode,
                           uint32_t *nb_slices, const uint32_t **offsets)
{
    static const uint8_t startcode_prefix[3] = { 0x0, 0x0, 0x1 };
    const size_t startcode_len = add_startcode ? sizeof(startcode_prefix) : 0;
    const int nb = *nb_slices;
    uint8_t *slices;
    uint32_t *slice_off;

    slice_off = av_fast_realloc(vp->slice_off, &vp->slice_off_max,
                                (nb + 1)*sizeof(slice_off));
    if (!slice_off)
        return AVERROR(ENOMEM);

    *offsets = vp->slice_off = slice_off;
    slice_off[nb] = vp->slices_size;

    slices = av_fast_realloc(vp->slices, &vp->slices_size_max,
                             vp->slices_size + size + startcode_len);
    if (!slices)
        return AVERROR(ENOMEM);

    vp->slices = slices;

    /* Startcode */
    memcpy(slices + vp->slices_size, startcode_prefix, startcode_len);

    /* Slice data */
    memcpy(slices + vp->slices_size + startcode_len, data, size);

    *nb_slices = nb + 1;
    vp->nb_slices++;
    vp->slices_size += startcode_len + size;

    return 0;
}

void ff_vk_decode_flush(AVCodecContext *avctx)
{
    FFVulkanDecodeContext *ctx = avctx->internal->hwaccel_priv_data;
    FFVulkanFunctions *vk = &ctx->s.vkfn;
    VkVideoBeginCodingInfoKHR decode_start = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR,
        .videoSession = ctx->common.session,
        .videoSessionParameters = ctx->empty_session_params,
    };
    VkVideoCodingControlInfoKHR decode_ctrl = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR,
        .flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR,
    };
    VkVideoEndCodingInfoKHR decode_end = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR,
    };

    VkCommandBuffer cmd_buf;
    FFVkExecContext *exec = ff_vk_exec_get(&ctx->exec_pool);
    ff_vk_exec_start(&ctx->s, exec);
    cmd_buf = exec->buf;

    vk->CmdBeginVideoCodingKHR(cmd_buf, &decode_start);
    vk->CmdControlVideoCodingKHR(cmd_buf, &decode_ctrl);
    vk->CmdEndVideoCodingKHR(cmd_buf, &decode_end);
    ff_vk_exec_submit(&ctx->s, exec);
}

static void host_map_buf_free(void *opaque, uint8_t *data)
{
    FFVulkanContext *ctx = opaque;
    FFVkVideoBuffer *buf = (FFVkVideoBuffer *)data;
    ff_vk_free_buf(ctx, &buf->buf);
    av_free(data);
}

int ff_vk_decode_frame(AVCodecContext *avctx,
                       AVFrame *pic,    FFVulkanDecodePicture *vp,
                       AVFrame *rpic[], FFVulkanDecodePicture *rvkp[])
{
    int err;
    VkResult ret;
    VkCommandBuffer cmd_buf;
    FFVkVideoBuffer *sd_buf;

    FFVulkanDecodeContext *ctx = avctx->internal->hwaccel_priv_data;
    FFVulkanFunctions *vk = &ctx->s.vkfn;

    /* Output */
    AVVkFrame *vkf = (AVVkFrame *)pic->buf[0]->data;

    /* Quirks */
    const int layered_dpb = ctx->layered_dpb;

    VkVideoSessionParametersKHR *par = (VkVideoSessionParametersKHR *)vp->session_params->data;
    VkVideoBeginCodingInfoKHR decode_start = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR,
        .videoSession = ctx->common.session,
        .videoSessionParameters = *par,
        .referenceSlotCount = vp->decode_info.referenceSlotCount,
        .pReferenceSlots = vp->decode_info.pReferenceSlots,
    };
    VkVideoEndCodingInfoKHR decode_end = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR,
    };

    VkImageMemoryBarrier2 img_bar[37];
    int nb_img_bar = 0;
    AVBufferRef *sd_ref = NULL;
    size_t data_size = FFALIGN(vp->slices_size, ctx->common.caps.minBitstreamBufferSizeAlignment);

    FFVkExecContext *exec = ff_vk_exec_get(&ctx->exec_pool);

    if (ctx->exec_pool.nb_queries) {
        int64_t prev_sub_res = 0;
        ff_vk_exec_wait(&ctx->s, exec);
        ret = ff_vk_exec_get_query(&ctx->s, exec, NULL, &prev_sub_res);
        if (ret != VK_NOT_READY && ret != VK_SUCCESS) {
            av_log(avctx, AV_LOG_ERROR, "Unable to perform query: %s!\n",
                   ff_vk_ret2str(ret));
            return AVERROR_EXTERNAL;
        }

        if (ret == VK_SUCCESS)
            av_log(avctx, prev_sub_res < 0 ? AV_LOG_ERROR : AV_LOG_DEBUG,
                   "Result of previous frame decoding: %li\n", prev_sub_res);
    }

    if (0) {
        size_t req_size;
        VkExternalMemoryBufferCreateInfo create_desc = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
            .pNext = &ctx->profile_list,
        };

        VkImportMemoryHostPointerInfoEXT import_desc = {
            .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
        };

        VkMemoryHostPointerPropertiesEXT p_props = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT,
        };

        /* Align slices pointer */
        import_desc.pHostPointer = (void *)FFALIGN((uintptr_t)vp->slices,
                                                   ctx->s.hprops.minImportedHostPointerAlignment);

        req_size = FFALIGN(data_size,
                           ctx->s.hprops.minImportedHostPointerAlignment);

        ret = vk->GetMemoryHostPointerPropertiesEXT(ctx->s.hwctx->act_dev,
                                                    import_desc.handleType,
                                                    import_desc.pHostPointer,
                                                    &p_props);

        if (ret == VK_SUCCESS) {
            sd_buf = av_mallocz(sizeof(*sd_buf));
            if (!sd_buf)
                return AVERROR(ENOMEM);

            err = ff_vk_create_buf(&ctx->s, &sd_buf->buf, req_size,
                                   &create_desc, &import_desc,
                                   VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
            if (err < 0) {
                av_free(sd_buf);
                return err; /* This shouldn't error out, unless it's critical */
            } else {
                size_t neg_offs = (uint8_t *)import_desc.pHostPointer - vp->slices;

                sd_ref = av_buffer_create((uint8_t *)sd_buf, sizeof(*sd_buf),
                                          host_map_buf_free, &ctx->s, 0);
                if (!sd_ref) {
                    ff_vk_free_buf(&ctx->s, &sd_buf->buf);
                    av_free(sd_buf);
                    return AVERROR(ENOMEM);
                }

                for (int i = 0; i < vp->nb_slices; i++)
                    vp->slice_off[i] -= neg_offs;

                sd_buf->mem = vp->slices;
            }
        }
    }

    if (!sd_ref) {
        err = ff_vk_video_get_buffer(&ctx->s, &ctx->common, &sd_ref,
                                     VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR,
                                     &ctx->profile_list, data_size);
        if (err < 0)
            return err;

        sd_buf = (FFVkVideoBuffer *)sd_ref->data;

        /* Copy the slices data to the buffer */
        memcpy(sd_buf->mem, vp->slices, vp->slices_size);
    }

    /* Flush if needed */
    if (!(sd_buf->buf.flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        VkMappedMemoryRange flush_buf = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = sd_buf->buf.mem,
            .offset = 0,
            .size = FFALIGN(vp->slices_size,
                            ctx->s.props.properties.limits.nonCoherentAtomSize),
        };

        ret = vk->FlushMappedMemoryRanges(ctx->s.hwctx->act_dev, 1, &flush_buf);
        if (ret != VK_SUCCESS) {
            av_log(avctx, AV_LOG_ERROR, "Failed to flush memory: %s\n",
                   ff_vk_ret2str(ret));
            av_buffer_unref(&sd_ref);
            return AVERROR_EXTERNAL;
        }
    }

    vp->decode_info.srcBuffer       = sd_buf->buf.buf;
    vp->decode_info.srcBufferOffset = 0;
    vp->decode_info.srcBufferRange  = data_size;

    /* Start command buffer recording */
    err = ff_vk_exec_start(&ctx->s, exec);
    if (err < 0)
        return err;
    cmd_buf = exec->buf;

    /* Slices */
    err = ff_vk_exec_add_dep_buf(&ctx->s, exec, &sd_ref, 1, 0);
    if (err < 0)
        return err;

    /* Parameters */
    err = ff_vk_exec_add_dep_buf(&ctx->s, exec, &vp->session_params, 1, 0);
    if (err < 0)
        return err;

    err = ff_vk_exec_add_dep_frame(&ctx->s, exec, pic,
                                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    if (err < 0)
        return err;

    err = ff_vk_exec_mirror_sem_value(&ctx->s, exec, &vp->sem, &vp->sem_value,
                                      pic);
    if (err < 0)
        return err;

    /* Output image - change layout, as it comes from a pool */
    img_bar[nb_img_bar] = (VkImageMemoryBarrier2) {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = NULL,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
        .srcAccessMask = vkf->access[0],
        .dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR,
        .oldLayout = vkf->layout[0],
        .newLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR,
        .srcQueueFamilyIndex = vkf->queue_family[0],
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = vkf->img[0],
        .subresourceRange = (VkImageSubresourceRange) {
            .aspectMask = vp->img_aspect,
            .layerCount = 1,
            .levelCount = 1,
        },
    };
    ff_vk_exec_update_frame(&ctx->s, exec, pic,
                            &img_bar[nb_img_bar], &nb_img_bar);

    /* Reference for the current image, if existing and not layered */
    if (vp->dpb_frame) {
        err = ff_vk_exec_add_dep_frame(&ctx->s, exec, vp->dpb_frame,
                                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
        if (err < 0)
            return err;
    }

    if (!layered_dpb) {
        /* All references (apart from the current) for non-layered refs */

        for (int i = 0; i < vp->decode_info.referenceSlotCount; i++) {
            AVFrame *ref_frame = rpic[i];
            FFVulkanDecodePicture *rvp = rvkp[i];
            AVFrame *ref = rvp->dpb_frame ? rvp->dpb_frame : ref_frame;

            err = ff_vk_exec_add_dep_frame(&ctx->s, exec, ref,
                                           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
            if (err < 0)
                return err;

            if (err == 0) {
                err = ff_vk_exec_mirror_sem_value(&ctx->s, exec,
                                                  &rvp->sem, &rvp->sem_value,
                                                  ref);
                if (err < 0)
                    return err;
            }

            if (!rvp->dpb_frame) {
                AVVkFrame *rvkf = (AVVkFrame *)ref->data[0];

                img_bar[nb_img_bar] = (VkImageMemoryBarrier2) {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                    .pNext = NULL,
                    .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    .srcAccessMask = rvkf->access[0],
                    .dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
                    .dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR |
                                     VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR,
                    .oldLayout = rvkf->layout[0],
                    .newLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR,
                    .srcQueueFamilyIndex = rvkf->queue_family[0],
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = rvkf->img[0],
                    .subresourceRange = (VkImageSubresourceRange) {
                        .aspectMask = rvp->img_aspect_ref,
                        .layerCount = 1,
                        .levelCount = 1,
                    },
                };
                ff_vk_exec_update_frame(&ctx->s, exec, ref,
                                        &img_bar[nb_img_bar], &nb_img_bar);
            }
        }
    } else if (vp->decode_info.referenceSlotCount ||
               vp->img_view_out != vp->img_view_ref) {
        /* Single barrier for a single layered ref */
        err = ff_vk_exec_add_dep_frame(&ctx->s, exec, ctx->layered_frame,
                                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
        if (err < 0)
            return err;
    }

    /* Change image layout */
    vk->CmdPipelineBarrier2KHR(cmd_buf, &(VkDependencyInfo) {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
            .pImageMemoryBarriers = img_bar,
            .imageMemoryBarrierCount = nb_img_bar,
        });

    /* Start, use parameters, decode and end decoding */
    vk->CmdBeginVideoCodingKHR(cmd_buf, &decode_start);

    /* Start status query TODO: remove check when radv gets support */
    if (ctx->exec_pool.nb_queries)
        vk->CmdBeginQuery(cmd_buf, ctx->exec_pool.query_pool, exec->query_idx + 0, 0);

    vk->CmdDecodeVideoKHR(cmd_buf, &vp->decode_info);

    /* End status query */
    if (ctx->exec_pool.nb_queries)
        vk->CmdEndQuery(cmd_buf, ctx->exec_pool.query_pool, exec->query_idx + 0);

    vk->CmdEndVideoCodingKHR(cmd_buf, &decode_end);

    /* End recording and submit for execution */
    return ff_vk_exec_submit(&ctx->s, exec);
}

void ff_vk_decode_free_frame(FFVulkanDecodeContext *ctx, FFVulkanDecodePicture *vp)
{
    FFVulkanFunctions *vk;
    VkSemaphoreWaitInfo sem_wait;

    // TODO: investigate why this happens
    if (!ctx) {
        av_freep(&vp->slices);
        av_freep(&vp->slice_off);
        av_frame_free(&vp->dpb_frame);
        return;
    }

    vk = &ctx->s.vkfn;

    /* We do not have to lock the frame here because we're not interested
     * in the actual current semaphore value, but only that it's later than
     * the time we submitted the image for decoding. */
    sem_wait = (VkSemaphoreWaitInfo) {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .pSemaphores = &vp->sem,
        .pValues = &vp->sem_value,
        .semaphoreCount = 1,
    };

    if (vp->sem)
        vk->WaitSemaphores(ctx->s.hwctx->act_dev, &sem_wait, UINT64_MAX);

    /* Free slices data
     * TODO: use a pool in the decode context instead to avoid per-frame allocs. */
    av_freep(&vp->slices);
    av_freep(&vp->slice_off);

    /* Destroy image view (out) */
    if (vp->img_view_out && vp->img_view_out != vp->img_view_ref)
        vk->DestroyImageView(ctx->s.hwctx->act_dev, vp->img_view_out, ctx->s.hwctx->alloc);

    /* Destroy image view (ref, unlayered) */
    if (vp->img_view_ref)
        vk->DestroyImageView(ctx->s.hwctx->act_dev, vp->img_view_ref, ctx->s.hwctx->alloc);

    av_frame_free(&vp->dpb_frame);
}

/* Since to even get decoder capabilities, we have to initialize quite a lot,
 * this function does initialization and saves it to hwaccel_priv_data if
 * available. */
static int vulkan_decode_check_init(AVCodecContext *avctx, AVBufferRef *frames_ref,
                                    int *width_align, int *height_align,
                                    enum AVPixelFormat *pix_fmt, VkFormat *vk_fmt,
                                    int *dpb_dedicate)
{
    VkResult ret;
    int err, max_level;
    const struct FFVkCodecMap *vk_codec = &ff_vk_codec_map[avctx->codec_id];
    AVHWFramesContext *frames = (AVHWFramesContext *)frames_ref->data;
    AVHWDeviceContext *device = (AVHWDeviceContext *)frames->device_ref->data;
    AVVulkanDeviceContext *hwctx = device->hwctx;
    enum AVPixelFormat avctx_format;
    enum AVPixelFormat context_format;
    enum AVPixelFormat provisional_format;
    VkFormat provisional_vk_fmt;
    int context_format_was_found;
    int base_profile, cur_profile = avctx->profile;

    int dedicated_dpb;
    int layered_dpb;

    FFVulkanDecodeContext *ctx = avctx->internal->hwaccel_priv_data;
    FFVulkanExtensions local_extensions = 0x0;
    FFVulkanExtensions *extensions = ctx ? &ctx->s.extensions : &local_extensions;
    FFVulkanFunctions local_vk = { 0 };
    FFVulkanFunctions *vk = ctx ? &ctx->s.vkfn : &local_vk;
    VkVideoCapabilitiesKHR local_caps = { 0 };
    VkVideoCapabilitiesKHR *caps = ctx ? &ctx->common.caps : &local_caps;
    VkVideoDecodeCapabilitiesKHR local_dec_caps = { 0 };
    VkVideoDecodeCapabilitiesKHR *dec_caps = ctx ? &ctx->dec_caps : &local_dec_caps;
    VkVideoDecodeUsageInfoKHR local_usage = { 0 };
    VkVideoDecodeUsageInfoKHR *usage = ctx ? &ctx->usage : &local_usage;
    VkVideoProfileInfoKHR local_profile = { 0 };
    VkVideoProfileInfoKHR *profile = ctx ? &ctx->profile : &local_profile;
    VkVideoProfileListInfoKHR local_profile_list = { 0 };
    VkVideoProfileListInfoKHR *profile_list = ctx ? &ctx->profile_list : &local_profile_list;

    VkVideoDecodeH264ProfileInfoKHR local_h264_profile = { 0 };
    VkVideoDecodeH264ProfileInfoKHR *h264_profile = ctx ? &ctx->h264_profile : &local_h264_profile;

    VkVideoDecodeH264ProfileInfoKHR local_h265_profile = { 0 };
    VkVideoDecodeH264ProfileInfoKHR *h265_profile = ctx ? &ctx->h265_profile : &local_h265_profile;

    VkPhysicalDeviceVideoFormatInfoKHR fmt_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR,
        .pNext = profile_list,
    };
    VkVideoDecodeH264CapabilitiesKHR h264_caps = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR,
    };
    VkVideoDecodeH265CapabilitiesKHR h265_caps = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR,
    };
    VkVideoFormatPropertiesKHR *ret_info;
    uint32_t nb_out_fmts = 0;

    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(avctx->sw_pix_fmt);
    if (!desc)
        return AVERROR(EINVAL);

    if (ctx && ctx->init)
        return 0;

    if (!vk_codec->decode_op)
        return AVERROR(EINVAL);

    *extensions = ff_vk_extensions_to_mask(hwctx->enabled_dev_extensions,
                                           hwctx->nb_enabled_dev_extensions);

    if (!(*extensions & FF_VK_EXT_VIDEO_DECODE_QUEUE)) {
        av_log(avctx, AV_LOG_ERROR, "Device does not support the %s extension!\n",
               VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME);
        return AVERROR(ENOSYS);
    } else if (!vk_codec->decode_extension) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported codec for Vulkan decoding: %s!\n",
               avcodec_get_name(avctx->codec_id));
        return AVERROR(ENOSYS);
    } else if (!(vk_codec->decode_extension & *extensions)) {
        av_log(avctx, AV_LOG_ERROR, "Device does not support decoding %s!\n",
               avcodec_get_name(avctx->codec_id));
        return AVERROR(ENOSYS);
    }

    err = ff_vk_load_functions(device, vk, *extensions, 1, 1);
    if (err < 0)
        return err;

repeat:
    if (avctx->codec_id == AV_CODEC_ID_H264) {
        base_profile = FF_PROFILE_H264_CONSTRAINED_BASELINE;
        dec_caps->pNext = &h264_caps;
        usage->pNext = h264_profile;
        h264_profile->sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
        h264_profile->stdProfileIdc = cur_profile;
        h264_profile->pictureLayout = avctx->field_order == AV_FIELD_PROGRESSIVE ?
                                      VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR :
                                      VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_INTERLACED_INTERLEAVED_LINES_BIT_KHR;
    } else if (avctx->codec_id == AV_CODEC_ID_H265) {
        base_profile = FF_PROFILE_HEVC_MAIN;
        dec_caps->pNext = &h265_caps;
        usage->pNext = h265_profile;
        h265_profile->sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR;
        h265_profile->stdProfileIdc = cur_profile;
    }

    usage->sType           = VK_STRUCTURE_TYPE_VIDEO_DECODE_USAGE_INFO_KHR;
    usage->videoUsageHints = VK_VIDEO_DECODE_USAGE_DEFAULT_KHR;

    profile->sType               = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
    /* adding a usage STILL breaks nvidia! */
    profile->pNext               = usage->pNext;
    profile->videoCodecOperation = vk_codec->decode_op;
    profile->chromaSubsampling   = ff_vk_subsampling_from_av_desc(desc);
    profile->lumaBitDepth        = ff_vk_depth_from_av_depth(desc->comp[0].depth);
    profile->chromaBitDepth      = profile->lumaBitDepth;

    profile_list->sType        = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
    profile_list->profileCount = 1;
    profile_list->pProfiles    = profile;

    /* Get the capabilities of the decoder for the given profile */
    caps->sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
    caps->pNext = dec_caps;
    dec_caps->sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR;
    /* dec_caps->pNext already filled in */

    ret = vk->GetPhysicalDeviceVideoCapabilitiesKHR(hwctx->phys_dev, profile,
                                                    caps);
    if (ret == VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR &&
        avctx->flags & AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH &&
        cur_profile != base_profile) {
        cur_profile = base_profile;
        av_log(avctx, AV_LOG_VERBOSE, "%s profile %s not supported, attempting "
               "again with profile %s\n",
               avcodec_get_name(avctx->codec_id),
               avcodec_profile_name(avctx->codec_id, avctx->profile),
               avcodec_profile_name(avctx->codec_id, base_profile));
        goto repeat;
    } else if (ret == VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR) {
        av_log(avctx, AV_LOG_VERBOSE, "Unable to initialize video session: "
               "%s profile \"%s\" not supported!\n",
               avcodec_get_name(avctx->codec_id),
               avcodec_profile_name(avctx->codec_id, cur_profile));
        return AVERROR(EINVAL);
    } else if (ret == VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR) {
        av_log(avctx, AV_LOG_VERBOSE, "Unable to initialize video session: "
               "format (%s) not supported!\n",
               av_get_pix_fmt_name(avctx->sw_pix_fmt));
        return AVERROR(EINVAL);
    } else if (ret == VK_ERROR_FEATURE_NOT_PRESENT ||
               ret == VK_ERROR_FORMAT_NOT_SUPPORTED) {
        return AVERROR(EINVAL);
    } else if (ret != VK_SUCCESS) {
        return AVERROR_EXTERNAL;
    }

    max_level = avctx->codec_id == AV_CODEC_ID_H264 ? h264_caps.maxLevelIdc :
                avctx->codec_id == AV_CODEC_ID_H265 ? h265_caps.maxLevelIdc :
                0;

    if (ctx) {
        av_log(avctx, AV_LOG_VERBOSE, "Decoder capabilities for %s profile \"%s\":\n",
               avcodec_get_name(avctx->codec_id),
               avcodec_profile_name(avctx->codec_id, avctx->profile));
        av_log(avctx, AV_LOG_VERBOSE, "    Maximum level: %i\n",
               max_level);
        av_log(avctx, AV_LOG_VERBOSE, "    Width: from %i to %i\n",
               caps->minCodedExtent.width, caps->maxCodedExtent.width);
        av_log(avctx, AV_LOG_VERBOSE, "    Height: from %i to %i\n",
               caps->minCodedExtent.height, caps->maxCodedExtent.height);
        av_log(avctx, AV_LOG_VERBOSE, "    Width alignment: %i\n",
               caps->pictureAccessGranularity.width);
        av_log(avctx, AV_LOG_VERBOSE, "    Height alignment: %i\n",
               caps->pictureAccessGranularity.height);
        av_log(avctx, AV_LOG_VERBOSE, "    Bitstream offset alignment: %"PRIu64"\n",
               caps->minBitstreamBufferOffsetAlignment);
        av_log(avctx, AV_LOG_VERBOSE, "    Bitstream size alignment: %"PRIu64"\n",
               caps->minBitstreamBufferSizeAlignment);
        av_log(avctx, AV_LOG_VERBOSE, "    Maximum references: %u\n",
               caps->maxDpbSlots);
        av_log(avctx, AV_LOG_VERBOSE, "    Maximum active references: %u\n",
               caps->maxActiveReferencePictures);
        av_log(avctx, AV_LOG_VERBOSE, "    Codec header version: %i.%i.%i (driver), %i.%i.%i (compiled)\n",
               CODEC_VER(caps->stdHeaderVersion.specVersion),
               CODEC_VER(dec_ext[avctx->codec_id]->specVersion));
        av_log(avctx, AV_LOG_VERBOSE, "    Decode modes:%s%s%s\n",
               dec_caps->flags ? "" :
                   " invalid",
               dec_caps->flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR ?
                   " reuse_dst_dpb" : "",
               dec_caps->flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_DISTINCT_BIT_KHR ?
                   " dedicated_dpb" : "");
        av_log(avctx, AV_LOG_VERBOSE, "    Capability flags:%s%s%s\n",
               caps->flags ? "" :
                   " none",
               caps->flags & VK_VIDEO_CAPABILITY_PROTECTED_CONTENT_BIT_KHR ?
                   " protected" : "",
               caps->flags & VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR ?
                   " separate_references" : "");
    }

    /* Check if decoding is possible with the given parameters */
    if (avctx->coded_width  < caps->minCodedExtent.width   ||
        avctx->coded_height < caps->minCodedExtent.height  ||
        avctx->coded_width  > caps->maxCodedExtent.width   ||
        avctx->coded_height > caps->maxCodedExtent.height)
        return AVERROR(EINVAL);

    if (!(avctx->hwaccel_flags & AV_HWACCEL_FLAG_IGNORE_LEVEL) &&
        avctx->level > max_level)
        return AVERROR(EINVAL);

    /* Some basic sanity checking */
    if (!(dec_caps->flags & (VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR |
                             VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_DISTINCT_BIT_KHR))) {
        av_log(avctx, AV_LOG_ERROR, "Buggy driver signals invalid decoding mode: neither "
               "VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR nor "
               "VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_DISTINCT_BIT_KHR are set!\n");
        return AVERROR_EXTERNAL;
    } else if ((dec_caps->flags & (VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR |
                                   VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_DISTINCT_BIT_KHR) ==
                                   VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR) &&
               !(caps->flags & VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR)) {
        av_log(avctx, AV_LOG_ERROR, "Cannot initialize Vulkan decoding session, buggy driver: "
               "VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR set "
               "but VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR is unset!\n");
        return AVERROR_EXTERNAL;
    }

    /* TODO: make dedicated_dpb tunable */
    dedicated_dpb = !(dec_caps->flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR);
    layered_dpb   = !(caps->flags & VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR);

    if (dedicated_dpb) {
        fmt_info.imageUsage = VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;
    } else {
        fmt_info.imageUsage = VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR |
                              VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT         |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
    }

    /* Get the format of the images necessary */
    ret = vk->GetPhysicalDeviceVideoFormatPropertiesKHR(hwctx->phys_dev,
                                                        &fmt_info,
                                                        &nb_out_fmts, NULL);
    if (ret == VK_ERROR_FORMAT_NOT_SUPPORTED ||
        (!nb_out_fmts && ret == VK_SUCCESS)) {
        return AVERROR(EINVAL);
    } else if (ret != VK_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Unable to get Vulkan format properties: %s!\n",
               ff_vk_ret2str(ret));
        return AVERROR_EXTERNAL;
    }

    ret_info = av_mallocz(sizeof(*ret_info)*nb_out_fmts);
    if (!ret_info)
        return AVERROR(ENOMEM);

    for (int i = 0; i < nb_out_fmts; i++)
        ret_info[i].sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;

    ret = vk->GetPhysicalDeviceVideoFormatPropertiesKHR(hwctx->phys_dev,
                                                        &fmt_info,
                                                        &nb_out_fmts, ret_info);
    if (ret == VK_ERROR_FORMAT_NOT_SUPPORTED ||
        (!nb_out_fmts && ret == VK_SUCCESS)) {
        av_free(ret_info);
        return AVERROR(EINVAL);
    } else if (ret != VK_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Unable to get Vulkan format properties: %s!\n",
               ff_vk_ret2str(ret));
        av_free(ret_info);
        return AVERROR_EXTERNAL;
    }

    if (ctx) {
        ctx->dedicated_dpb = dedicated_dpb;
        ctx->layered_dpb = layered_dpb;
        ctx->init = 1;
    }

    /* Find a format to use */
    *pix_fmt = provisional_format = AV_PIX_FMT_NONE;
    *vk_fmt  = provisional_vk_fmt = VK_FORMAT_UNDEFINED;
    avctx_format = avctx->sw_pix_fmt;
    context_format = frames->sw_format;
    context_format_was_found = 0;

    av_log(avctx, AV_LOG_DEBUG, "Pixel format list for decoding:\n");
    for (int i = 0; i < nb_out_fmts; i++) {
        int set = 0;
        enum AVPixelFormat tmp;
        if (ret_info[i].format == VK_FORMAT_UNDEFINED)
            continue;
        tmp = ff_vk_pix_fmt_from_vkfmt(ret_info[i].format);

        if (tmp != AV_PIX_FMT_NONE && !context_format_was_found) {
            if (provisional_format == AV_PIX_FMT_NONE) {
                provisional_format = tmp;
                provisional_vk_fmt = ret_info[i].format;
                set = 1;
            }
            if (tmp == context_format || tmp == avctx_format) {
                *pix_fmt = tmp;
                *vk_fmt  = ret_info[i].format;
                context_format_was_found |= tmp == context_format;
                set = 1;
            }
        }

        av_log(avctx, AV_LOG_DEBUG, "    %s%s (Vulkan ID: %i)\n",
               av_get_pix_fmt_name(tmp), set ? "*" : "", ret_info[i].format);
    }

    if (context_format != AV_PIX_FMT_NONE && !context_format_was_found) {
        av_log(avctx, AV_LOG_ERROR, "Frame context had a set pixel format of %s "
               "which the driver dost not list as available for decoding into!\n",
               av_get_pix_fmt_name(context_format));
        av_free(ret_info);
        return AVERROR(EINVAL);
    } else if (*pix_fmt == AV_PIX_FMT_NONE || *vk_fmt == VK_FORMAT_UNDEFINED &&
               provisional_format != AV_PIX_FMT_NONE) {
        *pix_fmt = provisional_format;
        *vk_fmt  = provisional_vk_fmt;
    } else if (*pix_fmt == AV_PIX_FMT_NONE || *vk_fmt == VK_FORMAT_UNDEFINED) {
        av_log(avctx, AV_LOG_ERROR, "No valid/compatible pixel format for decoding!\n");
        av_free(ret_info);
        return AVERROR(EINVAL);
    } else {
        av_free(ret_info);
        av_log(avctx, AV_LOG_VERBOSE, "Chosen frame pixfmt: %s (Vulkan ID: %i)\n",
               av_get_pix_fmt_name(*pix_fmt), *vk_fmt);
    }

    if (width_align)
        *width_align = caps->pictureAccessGranularity.width;
    if (height_align)
        *height_align = caps->pictureAccessGranularity.height;
    if (dpb_dedicate)
        *dpb_dedicate = dedicated_dpb;

    return 0;
}

int ff_vk_frame_params(AVCodecContext *avctx, AVBufferRef *hw_frames_ctx)
{
    VkFormat vkfmt;
    int err, width_align, height_align, dedicated_dpb;
    AVHWFramesContext *frames_ctx = (AVHWFramesContext*)hw_frames_ctx->data;
    AVVulkanFramesContext *hwfc = frames_ctx->hwctx;

    err = vulkan_decode_check_init(avctx, hw_frames_ctx, &width_align, &height_align,
                                   &frames_ctx->sw_format, &vkfmt,
                                   &dedicated_dpb);
    if (err < 0)
        return err;

    frames_ctx->width  = FFALIGN(avctx->coded_width, width_align);
    frames_ctx->height = FFALIGN(avctx->coded_height, height_align);
    frames_ctx->format = AV_PIX_FMT_VULKAN;

    hwfc->format[0] = vkfmt;
    hwfc->tiling    = VK_IMAGE_TILING_OPTIMAL;
    hwfc->usage     = VK_IMAGE_USAGE_TRANSFER_SRC_BIT         |
                      VK_IMAGE_USAGE_SAMPLED_BIT              |
                      VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR;

    if (avctx->internal->hwaccel_priv_data) {
        FFVulkanDecodeContext *ctx = avctx->internal->hwaccel_priv_data;
        hwfc->create_pnext = &ctx->profile_list;
    } else {
        /* TODO FIXME */
    }

    if (!dedicated_dpb)
        hwfc->usage |= VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;

    return err;
}

void ff_vk_decode_free_params(void *opaque, uint8_t *data)
{
    FFVulkanDecodeContext *ctx = opaque;
    FFVulkanFunctions *vk = &ctx->s.vkfn;
    VkVideoSessionParametersKHR *par = (VkVideoSessionParametersKHR *)data;
    vk->DestroyVideoSessionParametersKHR(ctx->s.hwctx->act_dev, *par,
                                         ctx->s.hwctx->alloc);
    av_free(par);
}

int ff_vk_decode_uninit(AVCodecContext *avctx)
{
    FFVulkanDecodeContext *ctx = avctx->internal->hwaccel_priv_data;
    FFVulkanContext *s = &ctx->s;
    FFVulkanFunctions *vk = &ctx->s.vkfn;

    /* Wait on and free execution pool */
    ff_vk_exec_pool_free(s, &ctx->exec_pool);

    /* Destroy layered view */
    if (ctx->layered_view)
        vk->DestroyImageView(s->hwctx->act_dev, ctx->layered_view, s->hwctx->alloc);

    /* This also frees all references from this pool */
    av_frame_free(&ctx->layered_frame);
    av_buffer_unref(&ctx->dpb_hwfc_ref);

    /* Destroy parameters */
    if (ctx->empty_session_params)
        vk->DestroyVideoSessionParametersKHR(s->hwctx->act_dev,
                                             ctx->empty_session_params,
                                             s->hwctx->alloc);

    ff_vk_video_common_uninit(s, &ctx->common);

    vk->DestroySamplerYcbcrConversion(s->hwctx->act_dev, ctx->yuv_sampler,
                                      s->hwctx->alloc);

    av_buffer_pool_uninit(&ctx->tmp_pool);

    ff_vk_uninit(s);

    return 0;
}

int ff_vk_decode_init(AVCodecContext *avctx)
{
    int err, qf, cxpos = 0, cypos = 0, nb_q = 0;
    VkResult ret;
    FFVulkanDecodeContext *ctx = avctx->internal->hwaccel_priv_data;
    FFVulkanContext *s = &ctx->s;
    FFVulkanFunctions *vk = &ctx->s.vkfn;

    VkVideoDecodeH264SessionParametersCreateInfoKHR h264_params = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR,
    };
    VkVideoDecodeH265SessionParametersCreateInfoKHR h265_params = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_CREATE_INFO_KHR,
    };
    VkVideoSessionParametersCreateInfoKHR session_params_create = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR,
        .pNext = avctx->codec_id == AV_CODEC_ID_H264 ? (void *)&h264_params :
                 avctx->codec_id == AV_CODEC_ID_HEVC ? (void *)&h265_params :
                 NULL,
    };
    VkVideoSessionCreateInfoKHR session_create = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR,
    };
    VkSamplerYcbcrConversionCreateInfo yuv_sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
        .components = ff_comp_identity_map,
        .ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY,
        .ycbcrRange = avctx->color_range == AVCOL_RANGE_MPEG, /* Ignored */
    };

    err = ff_decode_get_hw_frames_ctx(avctx, AV_HWDEVICE_TYPE_VULKAN);
    if (err < 0)
        return err;

    s->frames_ref = av_buffer_ref(avctx->hw_frames_ctx);
    s->frames = (AVHWFramesContext *)s->frames_ref->data;
    s->hwfc = s->frames->hwctx;

    s->device = (AVHWDeviceContext *)s->frames->device_ref->data;
    s->hwctx = s->device->hwctx;

    /* Get parameters, capabilities and final pixel/vulkan format */
    err = vulkan_decode_check_init(avctx, s->frames_ref, NULL, NULL,
                                   NULL, NULL, NULL);
    if (err < 0)
        goto fail;

    /* Load all properties */
    err = ff_vk_load_props(s);
    if (err < 0)
        goto fail;

    /* Create queue context */
    qf = ff_vk_qf_init(s, &ctx->qf_dec, VK_QUEUE_VIDEO_DECODE_BIT_KHR);

    /* Check for support */
    if (!(s->video_props[qf].videoCodecOperations &
          ff_vk_codec_map[avctx->codec_id].decode_op)) {
        av_log(avctx, AV_LOG_ERROR, "Decoding %s not supported on the given "
               "queue family %i!\n", avcodec_get_name(avctx->codec_id), qf);
        return AVERROR(EINVAL);
    }

    /* TODO: enable when stable and tested. */
    if (s->query_props[qf].queryResultStatusSupport)
        nb_q = 1;

    /* Create decode exec context.
     * 4 async contexts per thread seems like a good number. */
    err = ff_vk_exec_pool_init(s, &ctx->qf_dec, &ctx->exec_pool, 4*avctx->thread_count,
                               nb_q, VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR, 0,
                               &ctx->profile);
    if (err < 0)
        goto fail;

    session_create.pVideoProfile = &ctx->profile;
    session_create.flags = 0x0;
    session_create.queueFamilyIndex = s->hwctx->queue_family_decode_index;
    session_create.maxCodedExtent = ctx->common.caps.maxCodedExtent;
    session_create.maxDpbSlots = ctx->common.caps.maxDpbSlots;
    session_create.maxActiveReferencePictures = ctx->common.caps.maxActiveReferencePictures;
    session_create.pictureFormat = s->hwfc->format[0];
    session_create.referencePictureFormat = session_create.pictureFormat;
    session_create.pStdHeaderVersion = dec_ext[avctx->codec_id];

    err = ff_vk_video_common_init(avctx, s, &ctx->common, &session_create);
    if (err < 0)
        goto fail;

    /* Get sampler */
    av_chroma_location_enum_to_pos(&cxpos, &cypos, avctx->chroma_sample_location);
    yuv_sampler_info.xChromaOffset = cxpos >> 7;
    yuv_sampler_info.yChromaOffset = cypos >> 7;
    yuv_sampler_info.format = s->hwfc->format[0];
    ret = vk->CreateSamplerYcbcrConversion(s->hwctx->act_dev, &yuv_sampler_info,
                                           s->hwctx->alloc, &ctx->yuv_sampler);
    if (ret != VK_SUCCESS) {
        err = AVERROR_EXTERNAL;
        goto fail;
    }

    /* If doing an out-of-place decoding, create a DPB pool */
    if (ctx->dedicated_dpb) {
        AVHWFramesContext *dpb_frames;
        AVVulkanFramesContext *dpb_hwfc;

        ctx->dpb_hwfc_ref = av_hwframe_ctx_alloc(s->frames->device_ref);
        if (!ctx->dpb_hwfc_ref) {
            err = AVERROR(ENOMEM);
            goto fail;
        }

        dpb_frames = (AVHWFramesContext *)ctx->dpb_hwfc_ref->data;
        dpb_frames->format    = s->frames->format;
        dpb_frames->sw_format = s->frames->sw_format;
        dpb_frames->width     = s->frames->width;
        dpb_frames->height    = s->frames->height;

        dpb_hwfc = dpb_frames->hwctx;
        dpb_hwfc->create_pnext = &ctx->profile_list;
        dpb_hwfc->format[0]    = s->hwfc->format[0];
        dpb_hwfc->tiling       = VK_IMAGE_TILING_OPTIMAL;
        dpb_hwfc->usage        = VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR |
                                 VK_IMAGE_USAGE_SAMPLED_BIT; /* Shuts validator up. */

        if (ctx->layered_dpb)
            dpb_hwfc->nb_layers = ctx->common.caps.maxDpbSlots;

        err = av_hwframe_ctx_init(ctx->dpb_hwfc_ref);
        if (err < 0)
            goto fail;

        if (ctx->layered_dpb) {
            ctx->layered_frame = vk_get_dpb_pool(ctx);
            if (!ctx->layered_frame) {
                err = AVERROR(ENOMEM);
                goto fail;
            }

            err = vk_decode_create_view(ctx, &ctx->layered_view, &ctx->layered_aspect,
                                        (AVVkFrame *)ctx->layered_frame->data,
                                        s->hwfc->format[0]);
            if (err < 0)
                goto fail;
        }
    }

    session_params_create.videoSession = ctx->common.session;
    ret = vk->CreateVideoSessionParametersKHR(s->hwctx->act_dev, &session_params_create,
                                              s->hwctx->alloc, &ctx->empty_session_params);
    if (ret != VK_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Unable to create empty Vulkan video session parameters: %s!\n",
               ff_vk_ret2str(ret));
        return AVERROR_EXTERNAL;
    }

    ff_vk_decode_flush(avctx);

    av_log(avctx, AV_LOG_VERBOSE, "Vulkan decoder initialization sucessful\n");

    return 0;

fail:
    ff_vk_decode_uninit(avctx);

    return err;
}
