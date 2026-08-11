#include "media/svc/svc_descriptor.h"

#include "media/svc/svc_parser.h"

static int parse_vp9(const uint8_t *payload, size_t payload_len, sfu_svc_descriptor_t *out) {
  sfu_vp9_descriptor_t vp9;
  if (sfu_parse_vp9_descriptor(payload, payload_len, &vp9) != 0) {
    return -1;
  }

  out->sid = vp9.sid;
  out->tid = vp9.tid;
  out->p_bit = vp9.p_bit;
  out->u_bit = vp9.u_bit;
  out->d_bit = vp9.d_bit;
  out->b_bit = vp9.b_bit;
  out->e_bit = vp9.e_bit;
  out->l_bit = vp9.l_bit;
  return 0;
}

int sfu_svc_parse_descriptor(sfu_video_codec_t codec, const uint8_t *payload, size_t payload_len, sfu_svc_descriptor_t *out) {
  switch (codec) {
    case SFU_VIDEO_CODEC_VP9:
      return parse_vp9(payload, payload_len, out);
    case SFU_VIDEO_CODEC_VP8:
    case SFU_VIDEO_CODEC_AV1:
    case SFU_VIDEO_CODEC_NONE:
    default:
      return -1;
  }
}
