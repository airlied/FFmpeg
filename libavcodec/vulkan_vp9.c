/*
 * VP9 HW decode acceleration through VA API
 *
 * Copyright (C) 2015 Timo Rothenpieler <timo@rothenpieler.org>
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

#include "vp9dec.h"

#include "vulkan_decode.h"

const VkExtensionProperties ff_vk_dec_vp9_ext = {
    .extensionName = VK_STD_VULKAN_VIDEO_CODEC_VP9_DECODE_EXTENSION_NAME,
    .specVersion   = VK_STD_VULKAN_VIDEO_CODEC_VP9_DECODE_SPEC_VERSION,
};

typedef struct VP9VulkanDecodePicture {
    FFVulkanDecodeContext          *ctx;
    FFVulkanDecodePicture           vp;

    /* Current Picture */
    //    VkVideoDecodeVP9DpbSlotInfoMESA vkvp9_ref;
    StdVideoVP9MESAFrameHeader      vp9_frame_header;
    VkVideoDecodeVP9PictureInfoMESA vp9_pic_info;

    //    const AV1Frame *ref_src[8];
    //   VkVideoDecodeVP9DpbSlotInfoMESA vkvp9_refs[8];
} VP9VulkanDecodePicture;

static int vk_vp9_start_frame(AVCodecContext          *avctx,
                              av_unused const uint8_t *buffer,
                              av_unused uint32_t       size)
{
    const VP9SharedContext *h = avctx->priv_data;
    const VP9Frame *pic = &h->frames[CUR_FRAME];
    FFVulkanDecodeContext *ctx = avctx->internal->hwaccel_priv_data;
    VP9VulkanDecodePicture *v9p = pic->hwaccel_picture_private;
    FFVulkanDecodePicture *vp = &v9p->vp;
    const AVPixFmtDescriptor *pixdesc = av_pix_fmt_desc_get(avctx->sw_pix_fmt);

    v9p->vp9_pic_info = (VkVideoDecodeVP9PictureInfoMESA) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_PICTURE_INFO_MESA,
        .frame_header = &v9p->vp9_frame_header,
    };

    vp->decode_info = (VkVideoDecodeInfoKHR) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR,
        .pNext = &v9p->vp9_pic_info,
    };

    v9p->vp9_frame_header = (StdVideoVP9MESAFrameHeader) {
        .flags = (StdVideoVP9MESAFrameHeaderFlags) {
            .subsampling_x                = pixdesc->log2_chroma_w,
            .subsampling_y                = pixdesc->log2_chroma_h,
            .show_frame                   = !h->h.invisible,
            .error_resilient_mode         = h->h.errorres,
            .intra_only                   = h->h.intraonly,
            .refresh_frame_context        = h->h.refreshctx,
            .allow_high_precision_mv      = h->h.keyframe ? 0 : h->h.highprecisionmvs,
            .frame_parallel_decoding_mode = h->h.parallelmode,
        },
        .profile                           = h->h.profile,
        .bit_depth                         = h->h.bpp,
        .width                             = avctx->width,
        .height                            = avctx->height,
        .tile_rows_log2                    = h->h.tiling.log2_tile_rows,
        .tile_cols_log2                    = h->h.tiling.log2_tile_cols,
    };

    av_log(avctx, AV_LOG_DEBUG, "Created frame parameters");
    v9p->ctx = ctx;
    return 0;
}

static int vk_vp9_decode_slice(AVCodecContext *avctx,
                               const uint8_t  *data,
                               uint32_t        size)
{
    const VP9SharedContext *s = avctx->priv_data;
    VP9VulkanDecodePicture *v9p = s->frames[CUR_FRAME].hwaccel_picture_private;
    FFVulkanDecodePicture *vp = &v9p->vp;
    uint32_t slice_count;
    const uint32_t *slice_offsets;
    return ff_vk_decode_add_slice(avctx, vp, data, size, 0,
                                  &slice_count, &slice_offsets);
}

static int vk_vp9_end_frame(AVCodecContext *avctx)
{
    const VP9SharedContext *h = avctx->priv_data;
    const VP9Frame *pic = &h->frames[CUR_FRAME];
    VP9VulkanDecodePicture *v9p = pic->hwaccel_picture_private;
    FFVulkanDecodePicture *vp = &v9p->vp;
    FFVulkanDecodePicture *rvp[8] = { 0 };
    AVFrame *rav[8] = { 0 };
    return ff_vk_decode_frame(avctx, pic->tf.f, vp, rav, rvp);
}

static void vk_vp9_free_frame_priv(AVCodecContext *avctx, void *data)
{
    VP9VulkanDecodePicture *v9p = data;

    /* Free frame resources, this also destroys the session parameters. */
    ff_vk_decode_free_frame(v9p->ctx, &v9p->vp);

    /* Free frame context */
    av_free(v9p);
}

const AVHWAccel ff_vp9_vulkan_hwaccel = {
    .name = "vp9_vulkan",
    .type = AVMEDIA_TYPE_VIDEO,
    .id = AV_CODEC_ID_VP9,
    .pix_fmt = AV_PIX_FMT_VULKAN,
    .start_frame          = &vk_vp9_start_frame,
    .decode_slice         = &vk_vp9_decode_slice,
    .end_frame            = &vk_vp9_end_frame,
    .free_frame_priv      = &vk_vp9_free_frame_priv,
    .frame_priv_data_size = sizeof(VP9VulkanDecodePicture),
    .init                 = &ff_vk_decode_init,
    .flush                = &ff_vk_decode_flush,
    .uninit               = &ff_vk_decode_uninit,
    .frame_params         = &ff_vk_frame_params,
    .priv_data_size       = sizeof(FFVulkanDecodeContext),
    .caps_internal        = HWACCEL_CAP_ASYNC_SAFE | HWACCEL_CAP_THREAD_SAFE,
};
