/*
** Copyright 2015-2022 The Khronos Group Inc.
**
** SPDX-License-Identifier: Apache-2.0
*/

/*
** This header is NOT YET generated from the Khronos Vulkan XML API Registry.
**
*/

#ifndef VULKAN_VIDEO_CODEC_VP9STD_H_
#define VULKAN_VIDEO_CODEC_VP9STD_H_ 1

#ifdef __cplusplus
extern "C" {
#endif

#define vulkan_video_codec_vp9std 1

#define VK_MAKE_VIDEO_STD_VERSION(major, minor, patch)                         \
  ((((uint32_t)(major)) << 22) | (((uint32_t)(minor)) << 12) |                 \
   ((uint32_t)(patch)))
#define VK_STD_VULKAN_VIDEO_CODEC_VP9_DECODE_API_VERSION_0_0_1                 \
  VK_MAKE_VIDEO_STD_VERSION(0, 0, 1)
#define VK_STD_VULKAN_VIDEO_CODEC_VP9_DECODE_SPEC_VERSION                      \
  VK_STD_VULKAN_VIDEO_CODEC_VP9_DECODE_API_VERSION_0_0_1
#define VK_STD_VULKAN_VIDEO_CODEC_VP9_DECODE_EXTENSION_NAME                    \
  "VK_STD_vulkan_video_codec_vp9_decode"


typedef enum StdVideoVP9MESAProfile {
    STD_VIDEO_VP9_MESA_PROFILE_0,
    STD_VIDEO_VP9_MESA_PROFILE_1,
    STD_VIDEO_VP9_MESA_PROFILE_2,
    STD_VIDEO_VP9_MESA_PROFILE_3,
} StdVideoVP9MESAProfile;

typedef enum StdVideoVP9MESALevel {
    STD_VIDEO_VP9_MESA_LEVEL_1_0,
    STD_VIDEO_VP9_MESA_LEVEL_1_1,
    STD_VIDEO_VP9_MESA_LEVEL_2_0,
    STD_VIDEO_VP9_MESA_LEVEL_2_1,
    STD_VIDEO_VP9_MESA_LEVEL_3_0,
    STD_VIDEO_VP9_MESA_LEVEL_3_1,
    STD_VIDEO_VP9_MESA_LEVEL_4_0,
    STD_VIDEO_VP9_MESA_LEVEL_4_1,
    STD_VIDEO_VP9_MESA_LEVEL_5_0,
    STD_VIDEO_VP9_MESA_LEVEL_5_1,
    STD_VIDEO_VP9_MESA_LEVEL_5_2,
    STD_VIDEO_VP9_MESA_LEVEL_6_0,
    STD_VIDEO_VP9_MESA_LEVEL_6_1,
    STD_VIDEO_VP9_MESA_LEVEL_6_2,
} StdVideoVP9MESALevel;

typedef struct StdVideoVP9MESALoopFilterFlags {
    uint8_t delta_enabled;
    uint8_t delta_update;
} StdVideoVP9MESALoopFilterFlags;

#define MAX_REF_LF_DELTAS 4
#define MAX_MODE_LF_DELTAS 2
#define MAX_SEGMENTS 8
#define SEG_LVL_MAX 4

typedef struct StdVideoVP9MESALoopFilter {
    StdVideoVP9MESALoopFilterFlags flags;
    uint8_t level;
    uint8_t sharpness;
    int8_t ref_deltas[MAX_REF_LF_DELTAS];
    int8_t mode_deltas[MAX_MODE_LF_DELTAS];
} StdVideoVP9MESALoopFilter;

typedef struct StdVideoVP9MESAQuantization {
    uint8_t base_q_idx;
    int8_t delta_q_y_dc;
    int8_t delta_q_uv_dc;
    int8_t delta_q_uv_ac;
} StdVideoVP9MESAQuantization;

typedef struct StdVideoVP9MESASegmentationFlags {
    uint8_t enabled;
    uint8_t update_map;
    uint8_t temporal_update;
    uint8_t update_data;
    uint8_t abs_or_delta_update;
} StdVideoVP9MESASegmentationFlags;

typedef struct StdVideoVP9MESASegmentation {
    StdVideoVP9MESASegmentationFlags flags;
    uint8_t tree_probs[7];
    uint8_t seg_probs[3];
    uint8_t feature_enabled_bits[MAX_SEGMENTS];
    int16_t feature_data[MAX_SEGMENTS][SEG_LVL_MAX];
    uint8_t lvl_lookup[8][4][2];
} StdVideoVP9MESASegmentation;

typedef struct StdVideoVP9MESAFrameHeaderFlags {
    uint8_t subsampling_x;
    uint8_t subsampling_y;
    uint8_t show_frame;
    uint8_t error_resilient_mode;
    uint8_t intra_only;
    uint8_t refresh_frame_context;
    uint8_t allow_high_precision_mv;
    uint8_t frame_parallel_decoding_mode;
} StdVideoVP9MESAFrameHeaderFlags;

#define REFS_PER_FRAME 3

typedef struct StdVideoVP9MESAFrameHeader {
    StdVideoVP9MESAFrameHeaderFlags flags;
    uint8_t profile;
    uint8_t bit_depth;
    uint8_t color_space;
    uint8_t color_range;
    uint8_t frame_to_show_map_idx;
    uint8_t frame_type;
    uint32_t width;
    uint32_t height;
    uint32_t render_width;
    uint32_t render_height;
    uint8_t ref_frame_idx[REFS_PER_FRAME];
    uint8_t ref_frame_sign_bias[4];
    uint8_t interpolation_filter;
    uint8_t reset_frame_context;
    uint8_t frame_context_idx;
    StdVideoVP9MESALoopFilter loop_filter;
    StdVideoVP9MESAQuantization quantization;
    StdVideoVP9MESASegmentation segmentation;
    uint8_t tile_cols_log2;
    uint8_t tile_rows_log2;
    uint32_t uncompressed_header_size_in_bytes;
    uint32_t compressed_header_size_in_bytes;
} StdVideoVP9MESAFrameHeader;

typedef struct VkVideoDecodeVP9PictureInfoMESA {
    VkStructureType sType;
    const void *pNext;
    StdVideoVP9MESAFrameHeader *frame_header;
} VkVideoDecodeVP9PictureInfoMESA;

// TODO: what number goes here?
#define VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_PICTURE_INFO_MESA 100666000

typedef struct VkVideoDecodeVP9ProfileInfoMESA {
    VkStructureType sType;
    const void *pNext;
    StdVideoVP9MESAProfile profile;
} VkVideoDecodeVP9ProfileInfoMESA;

typedef struct VkVideoDecodeVP9CapabilitiesMESA {
    VkStructureType sType;
    const void *pNext;
    StdVideoVP9MESALevel level;
} VkVideoDecodeVP9CapabilitiesMESA;

#define VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_CAPABILITIES_MESA 100666001
#define VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_PROFILE_INFO_MESA 100666002

typedef struct StdVideoVP9MESAMvDeltaProbabilities {
    uint8_t joint[3];
    uint8_t sign[2];
    uint8_t klass[2][10];
    uint8_t class0_bit[2];
    uint8_t bits[2][10];
    uint8_t class0_fr[2][2][3];
    uint8_t fr[2][3];
    uint8_t class0_hp[2];
    uint8_t hp[2];
} StdVideoVP9MESAMvDeltaProbabilities;

typedef struct StdVideoVP9MESADeltaProbabilities {
   uint8_t tx_probs_8x8[2][1];
   uint8_t tx_probs_32x32[2][3];
   uint8_t tx_probs_16x16[2][2];
   uint8_t coef[4][2][2][6][6][3];
   uint8_t skip[3];
   uint8_t inter_mode[7][3];
   uint8_t interp_filter[4][2];
   uint8_t is_inter[4];
   uint8_t comp_mode[5];
   uint8_t single_ref[5][2];
   uint8_t comp_ref[5];
   uint8_t y_mode[4][9];
   uint8_t partition[16][3];
   StdVideoVP9MESAMvDeltaProbabilities mv;
} StdVideoVP9MESADeltaProbabilities;

typedef struct VkVideoDecodeVP9DeltaProbabilitiesMESA {
    VkStructureType sType;
    const void *pNext;
    StdVideoVP9MESADeltaProbabilities delta_probabilities;
} VkVideoDecodeVP9DeltaProbabilitiesMESA;

#define VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_DELTA_PROBABILITIES_MESA 100666004

#ifdef __cplusplus
}
#endif

#endif
