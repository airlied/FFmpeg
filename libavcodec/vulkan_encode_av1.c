#include "libavutil/opt.h"

#include "cbs.h"
#include "cbs_av1.h"
#include "codec_internal.h"
#include "version.h"

#include "vulkan_encode.h"

typedef struct VulkanEncodeAV1Context {
    FFVulkanEncodeContext vkenc;
    VkVideoEncodeAV1ProfileInfoMESA vkprofile;
    VkVideoEncodeAV1CapabilitiesMESA vkcaps;

    AV1RawOBU sh; /**< sequence header.*/
    AV1RawOBU fh; /**< frame header.*/

    CodedBitstreamContext      *cbc;
    CodedBitstreamFragment current_obu;

    StdVideoAV1MESAFrameHeader vk_fh;
    StdVideoAV1MESASequenceHeader vk_sh;

    int enable_128x128_superblock;

    /** user options */
    int profile;
    int tier;

} VulkanEncodeAV1Context;

typedef struct VulkanEncodeAV1Picture {

} VulkanEncodeAV1Picture;

static int vulkan_encode_av1_add_obu(AVCodecContext *avctx,
                                     CodedBitstreamFragment *au,
                                     uint8_t type,
                                     void *obu_unit)
{
    int ret;

    ret = ff_cbs_insert_unit_content(au, -1,
                                     type, obu_unit, NULL);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to add OBU unit: "
               "type = %d.\n", type);
        return ret;
    }

    return 0;
}

static int vulkan_encode_av1_write_obu(AVCodecContext *avctx,
                                       char *data, size_t *data_len,
                                       CodedBitstreamFragment *bs)
{
    VulkanEncodeAV1Context *priv = avctx->priv_data;
    int ret;

    ret = ff_cbs_write_fragment_data(priv->cbc, bs);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to write packed header.\n");
        return ret;
    }

    if (*data_len < bs->data_size) {
        av_log(avctx, AV_LOG_ERROR, "Access unit too large: %zu < %zu.\n",
               *data_len, bs->data_size);
        return AVERROR(ENOSPC);
    }

    memcpy(data, bs->data, bs->data_size);
    *data_len = bs->data_size;

    return 0;
}

static int vulkan_encode_av1_init_sequence_params(AVCodecContext *avctx)
{
    VulkanEncodeAV1Context *enc = avctx->priv_data;
    AV1RawOBU *sh_obu = &enc->sh;
    AV1RawSequenceHeader *sh = &sh_obu->obu.sequence_header;
    StdVideoAV1MESASequenceHeader *vkseq = &enc->vk_sh;
    const AVPixFmtDescriptor *desc;
    int ret;

    memset(sh_obu, 0, sizeof(*sh_obu));
    sh_obu->header.obu_type = AV1_OBU_SEQUENCE_HEADER;

    desc = av_pix_fmt_desc_get(avctx->sw_pix_fmt);
    //    av_assert0(desc);

    sh->seq_profile  = avctx->profile;

    if (!sh->seq_force_screen_content_tools)
        sh->seq_force_integer_mv = AV1_SELECT_INTEGER_MV;
    sh->frame_width_bits_minus_1  = av_log2(avctx->width);
    sh->frame_height_bits_minus_1 = av_log2(avctx->height);
    sh->max_frame_width_minus_1   = avctx->width - 1;
    sh->max_frame_height_minus_1  = avctx->height - 1;
    sh->enable_order_hint         = 1;
    sh->order_hint_bits_minus_1   = av_clip(av_log2(avctx->gop_size), 0, 7);
    sh->seq_tier[0]               = enc->tier;
    sh->use_128x128_superblock    = enc->enable_128x128_superblock;

    sh->color_config = (AV1RawColorConfig) {
        .high_bitdepth                  = desc->comp[0].depth == 8 ? 0 : 1,
        .color_primaries                = avctx->color_primaries,
        .transfer_characteristics       = avctx->color_trc,
        .matrix_coefficients            = avctx->colorspace,
        .color_description_present_flag = (avctx->color_primaries != AVCOL_PRI_UNSPECIFIED ||
                                           avctx->color_trc       != AVCOL_TRC_UNSPECIFIED ||
                                           avctx->colorspace      != AVCOL_SPC_UNSPECIFIED),
        .subsampling_x                  = desc->log2_chroma_w,
        .subsampling_y                  = desc->log2_chroma_h,
    };

    if (avctx->level != FF_LEVEL_UNKNOWN) {
        sh->seq_level_idx[0] = avctx->level;
    } else {
        //        const AV1LevelDescriptor *level;
        float framerate;

        if (avctx->framerate.num > 0 && avctx->framerate.den > 0)
            framerate = avctx->framerate.num / avctx->framerate.den;
        else
            framerate = 0;
#if 0
        level = ff_av1_guess_level(avctx->bit_rate, priv->tier,
                                   ctx->surface_width, ctx->surface_height,
                                   priv->tile_rows * priv->tile_cols,
                                   priv->tile_cols, framerate);
        if (level) {
            av_log(avctx, AV_LOG_VERBOSE, "Using level %s.\n", level->name);
            sh->seq_level_idx[0] = level->level_idx;
        } else {
            av_log(avctx, AV_LOG_VERBOSE, "Stream will not conform to "
                   "any normal level, using level 6.3 by default.\n");
            sh->seq_level_idx[0] = 19;
            sh->seq_tier[0] = 1;
        }
#endif
    }

    vkseq->seq_profile             = sh->seq_profile;
    vkseq->frame_width_bits_minus_1 = sh->frame_width_bits_minus_1;
    vkseq->frame_height_bits_minus_1 = sh->frame_height_bits_minus_1;
    vkseq->max_frame_width_minus_1 = sh->max_frame_width_minus_1;
    vkseq->max_frame_height_minus_1 = sh->max_frame_height_minus_1;
    vkseq->order_hint_bits_minus_1 = sh->order_hint_bits_minus_1;
    vkseq->seq_force_integer_mv = sh->seq_force_integer_mv;

    vkseq->flags.use_128x128_superblock = sh->use_128x128_superblock;
    vkseq->flags.enable_order_hint = sh->enable_order_hint;

    vkseq->color_config.subsampling_x = sh->color_config.subsampling_x;
    vkseq->color_config.subsampling_y = sh->color_config.subsampling_y;
    vkseq->color_config.bit_depth = sh->color_config.high_bitdepth ? 10 : 8;
    //    if (!(ctx->va_rc_mode & VA_RC_CQP)) {
    //        vkseq->bits_per_second = ctx->va_bit_rate;
    //        vkseq->seq_fields.bits.enable_cdef = sh->enable_cdef = 1;
    //    }

    return 0;
}

static int vulkan_encode_av1_write_sequence_header(AVCodecContext *avctx,
                                                   uint8_t *data, size_t *data_len)

{
    VulkanEncodeAV1Context *enc = avctx->priv_data;
    CodedBitstreamFragment *obu = &enc->current_obu;
    int ret;

    ret = vulkan_encode_av1_add_obu(avctx, obu, AV1_OBU_SEQUENCE_HEADER, &enc->sh);
    if (ret < 0)
        goto end;

    ret = vulkan_encode_av1_write_obu(avctx, data, data_len, obu);
    if (ret < 0)
        goto end;

end:
    ff_cbs_fragment_reset(obu);

    return ret;
}

static const FFCodecDefault vulkan_encode_av1_defaults[] = {
    { "b",              "0"   },
    { "g",              "120" },
    { NULL },
};

static const FFVulkanEncoder encoder = {
    .pic_priv_data_size = sizeof(VulkanEncodeAV1Picture),
    .write_stream_headers = vulkan_encode_av1_write_sequence_header,
};

static av_cold int vulkan_encode_av1_init(AVCodecContext *avctx)
{
    int err;
    VulkanEncodeAV1Context *enc = avctx->priv_data;

    enc->vkprofile = (VkVideoEncodeAV1ProfileInfoMESA) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_PROFILE_INFO_MESA,
    };

    enc->vkcaps = (VkVideoEncodeAV1CapabilitiesMESA) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_CAPABILITIES_MESA,
    };

    err = ff_cbs_init(&enc->cbc, AV_CODEC_ID_AV1, avctx);
    if (err < 0)
        return err;

    err = ff_vulkan_encode_init(avctx, &enc->vkenc, &enc->vkprofile, &enc->vkcaps,
                                &encoder, 0, 0);
    if (err < 0)
        return err;

    av_log(avctx, AV_LOG_VERBOSE, "AV1 encoder capabilities:\n");

    err = vulkan_encode_av1_init_sequence_params(avctx);
    if (err < 0)
        return err;
    return 0;
};


static av_cold int vulkan_encode_av1_close(AVCodecContext *avctx)
{
    VulkanEncodeAV1Context *enc = avctx->priv_data;
    ff_vulkan_encode_uninit(&enc->vkenc);
    return 0;
}

static int video_encode_av1_receive_packet(AVCodecContext *avctx, AVPacket *pkt)
{
    VulkanEncodeAV1Context *enc = avctx->priv_data;
    return ff_vulkan_encode_receive_packet(avctx, &enc->vkenc, pkt);
}

static void vulkan_encode_av1_flush(AVCodecContext *avctx)
{

}

#define OFFSET(x) offsetof(VulkanEncodeAV1Context, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM)
static const AVOption vulkan_encode_av1_options[] = {
    FF_VK_ENCODE_COMMON_OPTS

    { NULL },
};

static const AVClass vulkan_encode_av1_class = {
    .class_name = "av1_vulkan",
    .item_name  = av_default_item_name,
    .option     = vulkan_encode_av1_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_av1_vulkan_encoder = {
    .p.name = "av1_vulkan",
    CODEC_LONG_NAME("AV1 (Vulkan)"),
    .p.type = AVMEDIA_TYPE_VIDEO,
    .p.id = AV_CODEC_ID_AV1,
    .priv_data_size = sizeof(VulkanEncodeAV1Context),
    .init           = &vulkan_encode_av1_init,
    FF_CODEC_RECEIVE_PACKET_CB(&video_encode_av1_receive_packet),
    .flush          = &vulkan_encode_av1_flush,
    .close          = &vulkan_encode_av1_close,
    .p.priv_class   = &vulkan_encode_av1_class,
    .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_HARDWARE |
                      AV_CODEC_CAP_DR1 |
                      AV_CODEC_CAP_ENCODER_FLUSH /* | AV_CODEC_CAP_EXPERIMENTAL */,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
    .p.pix_fmts = (const enum AVPixelFormat[]) {
        AV_PIX_FMT_VULKAN,
        AV_PIX_FMT_NONE,
    },
    .defaults       = vulkan_encode_av1_defaults,
    .hw_configs     = ff_vulkan_encode_hw_configs,
    .p.wrapper_name = "vulkan",
};
