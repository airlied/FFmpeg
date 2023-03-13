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
    VkVideoDecodeVP9DpbSlotInfoMESA vkvp9_ref;
    StdVideoVP9MESAFrameHeader      vp9_frame_header;
    VkVideoDecodeVP9PictureInfoMESA vp9_pic_info;

    const VP9Frame *ref_src[8];
    VkVideoDecodeVP9DpbSlotInfoMESA vkvp9_refs[8];

    uint8_t frame_id_set;
    uint8_t frame_id;
} VP9VulkanDecodePicture;

static int vk_vp9_fill_pict(AVCodecContext *avctx, const VP9Frame **ref_src,
                            VkVideoReferenceSlotInfoKHR *ref_slot,      /* Main structure */
                            VkVideoPictureResourceInfoKHR *ref,         /* Goes in ^ */
                            VkVideoDecodeVP9DpbSlotInfoMESA *vkvp9_ref, /* Goes in ^ */
                            const VP9Frame *pic, int is_current,
                            int dpb_slot_index)
{
    FFVulkanDecodeContext *ctx = avctx->internal->hwaccel_priv_data;
    VP9VulkanDecodePicture *hp = pic->hwaccel_picture_private;
    FFVulkanDecodePicture *vkpic = &hp->vp;

    int err = ff_vk_decode_prepare_frame(ctx, pic->tf.f, vkpic, is_current,
                                         ctx->dedicated_dpb);
    if (err < 0)
        return err;

    *vkvp9_ref = (VkVideoDecodeVP9DpbSlotInfoMESA) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_DPB_SLOT_INFO_MESA,
        .frameIdx = hp->frame_id,
    };

    *ref = (VkVideoPictureResourceInfoKHR) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR,
        .codedOffset = (VkOffset2D){ 0, 0 },
        .codedExtent = (VkExtent2D){ pic->tf.f->width, pic->tf.f->height },
        .baseArrayLayer = (ctx->dedicated_dpb && ctx->layered_dpb) ?
                          dpb_slot_index : 0,
        .imageViewBinding = vkpic->img_view_ref,
    };

    *ref_slot = (VkVideoReferenceSlotInfoKHR) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR,
        .pNext = vkvp9_ref,
        .slotIndex = dpb_slot_index,
        .pPictureResource = ref,
    };

    if (ref_src)
        *ref_src = pic;

    return 0;
}

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
    int ref_count = 0;
    int err;

    if (!v9p->frame_id_set) {
        unsigned slot_idx = 0;
        for (unsigned i = 0; i < 32; i++) {
            if (!(ctx->frame_id_alloc_mask & (1 << i))) {
                slot_idx = i;
                break;
            }
        }
        v9p->frame_id = slot_idx;
        v9p->frame_id_set = 1;
        ctx->frame_id_alloc_mask |= (1 << slot_idx);
    }

    for (unsigned i = 0; i < 8; i++) {
        if (h->refs[i].tf.f->pict_type == AV_PICTURE_TYPE_NONE)
            continue;

        err = vk_vp9_fill_pict(avctx, &v9p->ref_src[i], &vp->ref_slots[i], &vp->refs[i],
                               &v9p->vkvp9_refs[i],
                               &h->refs[i], 0, i);
        if (err < 0)
            return err;
        ref_count++;
    }
    err = vk_vp9_fill_pict(avctx, NULL, &vp->ref_slot, &vp->ref,
                           &v9p->vkvp9_ref,
                           pic, 1, 8);
    if (err < 0)
        return err;

    v9p->vp9_pic_info = (VkVideoDecodeVP9PictureInfoMESA) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_PICTURE_INFO_MESA,
        .frame_header = &v9p->vp9_frame_header,
        .use_prev_in_find_mv_refs = h->h.use_last_frame_mvs,
    };

    vp->decode_info = (VkVideoDecodeInfoKHR) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR,
        .pNext = &v9p->vp9_pic_info,
        .pSetupReferenceSlot = &vp->ref_slot,
        .referenceSlotCount = ref_count,
        .pReferenceSlots = vp->ref_slots,
        .dstPictureResource = (VkVideoPictureResourceInfoKHR) {
            .sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR,
            .codedOffset = (VkOffset2D){ 0, 0 },
            .codedExtent = (VkExtent2D){ pic->tf.f->width, pic->tf.f->height },
            .baseArrayLayer = 0,
            .imageViewBinding = vp->img_view_out,
        },
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
	.quantization = (StdVideoVP9MESAQuantization) {
            .base_q_idx = h->h.yac_qi,
            .delta_q_y_dc = h->h.ydc_qdelta,
            .delta_q_uv_dc = h->h.uvdc_qdelta,
            .delta_q_uv_ac = h->h.uvac_qdelta,
	},
        .loop_filter = (StdVideoVP9MESALoopFilter) {
            .flags = (StdVideoVP9MESALoopFilterFlags) {
                .delta_enabled = h->h.lf_delta.enabled,
                .delta_update = h->h.lf_delta.updated,
            },
            .level = h->h.filter.level,
            .sharpness = h->h.filter.sharpness,
            .ref_deltas[0] = h->h.lf_delta.ref[0],
            .ref_deltas[1] = h->h.lf_delta.ref[1],
            .ref_deltas[2] = h->h.lf_delta.ref[2],
            .ref_deltas[3] = h->h.lf_delta.ref[3],
            .mode_deltas[0] = h->h.lf_delta.mode[0],
            .mode_deltas[1] = h->h.lf_delta.mode[1],
        },
        .segmentation = (StdVideoVP9MESASegmentation) {
            .flags = (StdVideoVP9MESASegmentationFlags) {
                .enabled = h->h.segmentation.enabled,
                .temporal_update = h->h.segmentation.temporal,
                .update_map = h->h.segmentation.update_map,
            },
        },
        .frame_type                        = !h->h.keyframe,
        .profile                           = h->h.profile,
        .bit_depth                         = h->h.bpp,
        .interpolation_filter              = h->h.filtermode ^ (h->h.filtermode <= 1),
        .width                             = avctx->width,
        .height                            = avctx->height,
        .tile_rows_log2                    = h->h.tiling.log2_tile_rows,
        .tile_cols_log2                    = h->h.tiling.log2_tile_cols,
        .uncompressed_header_size_in_bytes = h->h.uncompressed_header_size,
        .compressed_header_size_in_bytes   = h->h.compressed_header_size,
        .reset_frame_context               = h->h.resetctx,
        .frame_context_idx                 = h->h.framectxid,

        .ref_frame_idx[0] = h->h.refidx[0],
        .ref_frame_idx[1] = h->h.refidx[1],
        .ref_frame_idx[2] = h->h.refidx[2],

        .ref_frame_sign_bias[0] = h->h.signbias[0],
        .ref_frame_sign_bias[1] = h->h.signbias[1],
        .ref_frame_sign_bias[2] = h->h.signbias[2],
    };

    for (unsigned i = 0; i < 8; i++) {
        memcpy(&v9p->vp9_frame_header.segmentation.lvl_lookup[i], h->h.segmentation.feat[i].lflvl, 8);
    }

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

    for (int i = 0; i < vp->decode_info.referenceSlotCount; i++) {
        VP9VulkanDecodePicture *rv9p = v9p->ref_src[i]->hwaccel_picture_private;
        rvp[i] = &rv9p->vp;
        rav[i] = v9p->ref_src[i]->tf.f;
    }
    return ff_vk_decode_frame(avctx, pic->tf.f, vp, rav, rvp);
}

static void vk_vp9_free_frame_priv(AVCodecContext *avctx, void *data)
{
    VP9VulkanDecodePicture *v9p = data;
    
    if (v9p->frame_id_set)
        v9p->ctx->frame_id_alloc_mask &= ~(1 << v9p->frame_id);

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
