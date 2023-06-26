#include "libavutil/opt.h"

#include "cbs.h"
#include "cbs_av1.h"
#include "codec_internal.h"
#include "version.h"

#include "vulkan_encode.h"

typedef struct VulkanEncodeAV1Context {
    FFVulkanEncodeContext vkenc;

    CodedBitstreamContext      *cbc;
} VulkanEncodeAV1Context;

typedef struct VulkanEncodeAV1Picture {

} VulkanEncodeAV1Picture;

static const FFCodecDefault vulkan_encode_av1_defaults[] = {
    { "b",              "0"   },
    { "g",              "120" },
    { NULL },
};

static const FFVulkanEncoder encoder = {
    .pic_priv_data_size = sizeof(VulkanEncodeAV1Picture),
};

static av_cold int vulkan_encode_av1_init(AVCodecContext *avctx)
{
    int err;
    VulkanEncodeAV1Context *enc = avctx->priv_data;

    err = ff_cbs_init(&enc->cbc, AV_CODEC_ID_AV1, avctx);
    if (err < 0)
        return err;
    
    err = ff_vulkan_encode_init(avctx, &enc->vkenc, NULL, NULL,
                                &encoder, 0, 0);
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
