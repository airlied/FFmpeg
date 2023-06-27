#include "libavutil/opt.h"

#include "cbs.h"
#include "cbs_av1.h"
#include "codec_internal.h"
#include "version.h"
#include "av1_profile_level.h"

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
    int surface_width;
    int surface_height;

    int gop_size;
    /** user options */
    int profile;
    int tier;
    int level;
    int tile_cols;
    int tile_rows;
} VulkanEncodeAV1Context;

typedef struct VulkanEncodeAV1Picture {
    StdVideoAV1MESAFrameHeader vkav1_fh;
    VkVideoEncodeAV1PictureInfoMESA vkav1pic_info;
    //    VkVideoEncodeAV1RateControlInfoEXT vkrc_info;
    //    VkVideoEncodeAV1RateControlLayerInfoEXT vkrc_layer_info;
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
        const AV1LevelDescriptor *level;
        float framerate;

        if (avctx->framerate.num > 0 && avctx->framerate.den > 0)
            framerate = avctx->framerate.num / avctx->framerate.den;
        else
            framerate = 0;
        level = ff_av1_guess_level(avctx->bit_rate, enc->tier,
                                   enc->surface_width, enc->surface_height,
                                   enc->tile_rows * enc->tile_cols,
                                   enc->tile_cols, framerate);
        if (level) {
            av_log(avctx, AV_LOG_VERBOSE, "Using level %s.\n", level->name);
            sh->seq_level_idx[0] = level->level_idx;
        } else {
            av_log(avctx, AV_LOG_VERBOSE, "Stream will not conform to "
                   "any normal level, using level 6.3 by default.\n");
            sh->seq_level_idx[0] = 19;
            sh->seq_tier[0] = 1;
        }
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

static av_cold int vulkan_encode_av1_create_session(AVCodecContext *avctx)
{
    VkResult ret;
    VulkanEncodeAV1Context *enc = avctx->priv_data;
    FFVulkanFunctions *vk = &enc->vkenc.s.vkfn;

    VkVideoEncodeAV1SessionParametersAddInfoMESA av1_params_info;
    VkVideoEncodeAV1SessionParametersCreateInfoMESA av1_params;
    VkVideoSessionParametersCreateInfoKHR session_params_create;

    av1_params_info = (VkVideoEncodeAV1SessionParametersAddInfoMESA) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_SESSION_PARAMETERS_ADD_INFO_MESA,
        .sequence_header = &enc->vk_sh,
    };

    av1_params = (VkVideoEncodeAV1SessionParametersCreateInfoMESA) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_SESSION_PARAMETERS_CREATE_INFO_MESA,
        .pParametersAddInfo = &av1_params_info,
    };

    session_params_create = (VkVideoSessionParametersCreateInfoKHR) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR,
        .pNext = &av1_params,
        .videoSession = enc->vkenc.common.session,
        .videoSessionParametersTemplate = NULL,
    };

    ret = vk->CreateVideoSessionParametersKHR(enc->vkenc.s.hwctx->act_dev, &session_params_create,
                                              enc->vkenc.s.hwctx->alloc, &enc->vkenc.session_params);
    if (ret != VK_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Unable to create Vulkan video session parameters: %s!\n",
               ff_vk_ret2str(ret));
        return AVERROR_EXTERNAL;
    }

    return 0;
}

static int vulkan_encode_av1_init_pic_headers(AVCodecContext *avctx,
                                              FFVulkanEncodePicture *pic)
{
    VulkanEncodeAV1Context    *enc = avctx->priv_data;
    VulkanEncodeAV1Picture   *av1pic = pic->priv_data;
    FFVulkanEncodePicture     *prev = pic->prev;
    VulkanEncodeAV1Picture  *av1prev = prev ? prev->priv_data : NULL;
    AV1RawOBU                    *fh_obu = &enc->fh;
    AV1RawFrameHeader                *frame_header = &fh_obu->obu.frame.header;
    switch (pic->type) {
    case FF_VK_FRAME_I:
    case FF_VK_FRAME_KEY:
        frame_header->frame_type = AV1_FRAME_KEY;
        frame_header->refresh_frame_flags = 0xff;
        frame_header->base_q_idx = 0;
        break;
    case FF_VK_FRAME_P:
        frame_header->frame_type = AV1_FRAME_INTER;
        break;
    case FF_VK_FRAME_B:
        frame_header->frame_type = AV1_FRAME_INTER;
        break;
    };
    av1pic->vkav1_fh = (StdVideoAV1MESAFrameHeader) {
        .flags = (StdVideoAV1MESAFrameHeaderFlags) {
            .error_resilient_mode = frame_header->error_resilient_mode,
            .disable_cdf_update = frame_header->disable_cdf_update,
            .use_superres = frame_header->use_superres,
            .render_and_frame_size_different = frame_header->render_and_frame_size_different,
            .allow_screen_content_tools = frame_header->allow_screen_content_tools,
            .is_filter_switchable = frame_header->is_filter_switchable,
            .force_integer_mv = frame_header->force_integer_mv,
            .frame_size_override_flag = frame_header->frame_size_override_flag,
            .buffer_removal_time_present_flag = frame_header->buffer_removal_time_present_flag,
            .allow_intrabc = frame_header->allow_intrabc,
            .frame_refs_short_signaling = frame_header->frame_refs_short_signaling,
            .allow_high_precision_mv = frame_header->allow_high_precision_mv,
            .is_motion_mode_switchable = frame_header->is_motion_mode_switchable,
            .use_ref_frame_mvs = frame_header->use_ref_frame_mvs,
            .disable_frame_end_update_cdf = frame_header->disable_frame_end_update_cdf,
            .allow_warped_motion = frame_header->allow_warped_motion,
            .reduced_tx_set = frame_header->reduced_tx_set,
            .reference_select = frame_header->reference_select,
            .skip_mode_present = frame_header->skip_mode_present,
            .delta_q_present = frame_header->delta_q_present,
        },
        .frame_type = frame_header->frame_type,
        .order_hint = frame_header->order_hint,
        .frame_width_minus_1 = frame_header->frame_width_minus_1,
        .frame_height_minus_1 = frame_header->frame_height_minus_1,
        .coded_denom = frame_header->coded_denom,
        .render_width_minus_1 = frame_header->render_width_minus_1,
        .render_height_minus_1 = frame_header->render_height_minus_1,
        .refresh_frame_flags = frame_header->refresh_frame_flags,
        .interpolation_filter = frame_header->interpolation_filter,
        .tx_mode = frame_header->tx_mode,
    };
    av1pic->vkav1pic_info = (VkVideoEncodeAV1PictureInfoMESA) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_PICTURE_INFO_MESA,
        .pNext = NULL,
        .frame_header = &av1pic->vkav1_fh,
    };

    pic->codec_info = &av1pic->vkav1pic_info;
    return 0;
}

static const FFCodecDefault vulkan_encode_av1_defaults[] = {
    { "b",              "0"   },
    { "g",              "120" },
    { NULL },
};

static const FFVulkanEncoder encoder = {
    .pic_priv_data_size = sizeof(VulkanEncodeAV1Picture),
    .write_stream_headers = vulkan_encode_av1_write_sequence_header,
    .init_pic_headers = vulkan_encode_av1_init_pic_headers,
};

static av_cold int vulkan_encode_av1_init(AVCodecContext *avctx)
{
    int err;
    VulkanEncodeAV1Context *enc = avctx->priv_data;

    if (avctx->profile == FF_PROFILE_UNKNOWN)
        avctx->profile = enc->profile;
    enc->vkprofile = (VkVideoEncodeAV1ProfileInfoMESA) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_PROFILE_INFO_MESA,
    };

    enc->vkcaps = (VkVideoEncodeAV1CapabilitiesMESA) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_CAPABILITIES_MESA,
    };

    err = ff_cbs_init(&enc->cbc, AV_CODEC_ID_AV1, avctx);
    if (err < 0)
        return err;

    enc->gop_size = avctx->gop_size;
    enc->vkenc.gop_size = enc->gop_size;
    err = ff_vulkan_encode_init(avctx, &enc->vkenc, &enc->vkprofile, &enc->vkcaps,
                                &encoder, 0, 0);
    if (err < 0)
        return err;

    av_log(avctx, AV_LOG_VERBOSE, "AV1 encoder capabilities:\n");

    enc->surface_width  = FFALIGN(avctx->width,  128);
    enc->surface_height = FFALIGN(avctx->height, 128);
    err = vulkan_encode_av1_init_sequence_params(avctx);
    if (err < 0)
        return err;

    err = vulkan_encode_av1_create_session(avctx);
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
    { "profile", "Set profile (seq_profile)",
      OFFSET(profile), AV_OPT_TYPE_INT,
      { .i64 = FF_PROFILE_AV1_MAIN }, FF_PROFILE_UNKNOWN, 0xff, FLAGS, "profile" },

#define PROFILE(name, value)  name, NULL, 0, AV_OPT_TYPE_CONST, \
    { .i64 = value }, 0, 0, FLAGS, "profile"
    { PROFILE("main",               FF_PROFILE_AV1_MAIN) },
    { PROFILE("high",               FF_PROFILE_AV1_HIGH) },
    { PROFILE("professional",       FF_PROFILE_AV1_PROFESSIONAL) },
#undef PROFILE

    { "level", "Set level (seq_level_idx)",
      OFFSET(level), AV_OPT_TYPE_INT,
      { .i64 = FF_LEVEL_UNKNOWN }, FF_LEVEL_UNKNOWN, 0x1f, FLAGS, "level" },

#define LEVEL(name, value) name, NULL, 0, AV_OPT_TYPE_CONST, \
      { .i64 = value }, 0, 0, FLAGS, "level"
    { LEVEL("2.0",  0) },
    { LEVEL("2.1",  1) },
    { LEVEL("3.0",  4) },
    { LEVEL("3.1",  5) },
    { LEVEL("4.0",  8) },
    { LEVEL("4.1",  9) },
    { LEVEL("5.0", 12) },
    { LEVEL("5.1", 13) },
    { LEVEL("5.2", 14) },
    { LEVEL("5.3", 15) },
    { LEVEL("6.0", 16) },
    { LEVEL("6.1", 17) },
    { LEVEL("6.2", 18) },
    { LEVEL("6.3", 19) },
#undef LEVEL
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
