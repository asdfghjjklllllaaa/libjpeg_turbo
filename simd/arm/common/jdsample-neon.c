/*
 * jdsample-neon.c - upsampling (Arm NEON)
 *
 * Copyright 2019 The Chromium Authors. All Rights Reserved.
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

#define JPEG_INTERNALS
#include "../../../jinclude.h"
#include "../../../jpeglib.h"
#include "../../../jsimd.h"
#include "../../../jdct.h"
#include "../../../jsimddct.h"
#include "../../jsimd.h"

#include <arm_neon.h>

/*
 * The diagram below shows a grid-window of samples (luma or chroma) produced
 * by h2v2 downsampling.
 *
 *                  s0        s1
 *             +---------+---------+
 *             | p0   p1 | p2   p3 |
 *     r0      |         |         |
 *             | p4   p5 | p6   p7 |
 *             +---------+---------+
 *             | p8   p9 | p10  p11|
 *     r1      |         |         |
 *             | p12  p13| p14  p15|
 *             +---------+---------+
 *             | p16  p17| p18  p19|
 *     r2      |         |         |
 *             | p20  p21| p22  p23|
 *             +---------+---------+
 *
 * Every sample contains four of the original pixel channel values. The pixels'
 * channel values are centred at positions p0, p1, p2,..., p23 above. For a
 * given grid-window position, r1 is always used to denote the row of samples
 * containing the pixel channel values we are computing. For the top row of
 * pixel channel values in r1 (p8-p11), the nearest neighbouring samples are in
 * the row above - denoted by r0. Likewise, for the bottom row of pixels in r1
 * (p12-p15), the nearest neighbouring samples are in the row below - denoted
 * by r2.
 *
 * To compute the pixel channel values of the original image, we proportionally
 * blend the sample containing the pixel centre with the nearest neighbouring
 * samples in each row, column and diagonal.
 *
 * There are three cases to consider:
 *
 * 1) The first pixel in this row of the original image.
 *    Pixel channel value p8 only contains components from sample column s0.
 *    Its value is computed by blending samples s0r1 and s0r0 in the ratio 3:1.
 * 2) The last pixel in this row of the original image.
 *    Pixel channel value p11 only contains components from sample column s1.
 *    Its value is computed by blending samples s1r1 and s1r0 in the ratio 3:1.
 * 3) General case (all other pixels in the row).
 *    Apart from the first and last pixels, every other pixel channel value in
 *    the row contains components from samples in adjacent columns.
 *
 *    For example, the pixel centred at p9 would be computed as follows:
 *        (9/16 * s0r1) + (3/16 * s0r0) + (3/16 * s1r1) + (1/16 * s1r0)
 *
 *    This can be broken down into two steps:
 *    1) Blend samples vertically in columns s0 and s1 in the ratio 3:1:
 *        s0colsum = 3/4 * s0r1 + 1/4 * s0r0
 *        s1colsum = 3/4 * s1r1 + 1/4 * s1r0
 *    2) Blend the already-blended columns in the ratio 3:1:
 *        p9 = 3/4 * s0colsum + 1/4 * s1colsum
 *
 * The bottom row of pixel channel values in row r1 can be computed in the same
 * way for each of the three cases, only using samples in row r2 instead of row
 * r0 - as r2 is the nearest neighbouring row.
 */

void jsimd_h2v2_fancy_upsample_neon(int max_v_samp_factor,
                                    JDIMENSION downsampled_width,
                                    JSAMPARRAY input_data,
                                    JSAMPARRAY *output_data_ptr)
{
  JSAMPARRAY output_data = *output_data_ptr;
  JSAMPROW inptr0, inptr1, inptr2, outptr0, outptr1;
  int inrow, outrow;
  /* Setup constants. */
  const uint16x8_t seven_u16 = vdupq_n_u16(7);
  const uint8x8_t three_u8 = vdup_n_u8(3);
  const uint16x8_t three_u16 = vdupq_n_u16(3);

  inrow = outrow = 0;
  while (outrow < max_v_samp_factor) {
    inptr0 = input_data[inrow - 1];
    inptr1 = input_data[inrow];
    inptr2 = input_data[inrow + 1];
    /* Suffixes 0 and 1 denote the top and bottom rows of output pixels */
    /* respectively. */
    outptr0 = output_data[outrow++];
    outptr1 = output_data[outrow++];

    /* Case 1: first pixel channel value in this row of original image. */
    int s0colsum0 = GETJSAMPLE(*inptr1) * 3 + GETJSAMPLE(*inptr0);
    *outptr0 = (JSAMPLE)((s0colsum0 * 4 + 8) >> 4);
    int s0colsum1 = GETJSAMPLE(*inptr1) * 3 + GETJSAMPLE(*inptr2);
    *outptr1 = (JSAMPLE)((s0colsum1 * 4 + 8) >> 4);

    /* General case as described above. */
    /* Step 1: Blend samples vertically in columns s0 and s1. */
    /* Leave the divide by 4 to the end when it can be done for both */
    /* dimensions at once, right-shifting by 4. */

    /* Load and compute s0colsum0 and s0colsum1. */
    uint8x16_t s0r0 = vld1q_u8(inptr0);
    uint8x16_t s0r1 = vld1q_u8(inptr1);
    uint8x16_t s0r2 = vld1q_u8(inptr2);
    /* Multiplication makes vectors twice as wide: '_l' and '_h' suffixes */
    /* denote low half and high half respectively. */
    uint16x8_t s0colsum0_l = vmlal_u8(vmovl_u8(vget_low_u8(s0r0)),
                                      vget_low_u8(s0r1), three_u8);
    uint16x8_t s0colsum0_h = vmlal_u8(vmovl_u8(vget_high_u8(s0r0)),
                                      vget_high_u8(s0r1), three_u8);
    uint16x8_t s0colsum1_l = vmlal_u8(vmovl_u8(vget_low_u8(s0r2)),
                                      vget_low_u8(s0r1), three_u8);
    uint16x8_t s0colsum1_h = vmlal_u8(vmovl_u8(vget_high_u8(s0r2)),
                                      vget_high_u8(s0r1), three_u8);
    /* Load and compute s1colsum0 and s1colsum1. */
    uint8x16_t s1r0 = vld1q_u8(inptr0 + 1);
    uint8x16_t s1r1 = vld1q_u8(inptr1 + 1);
    uint8x16_t s1r2 = vld1q_u8(inptr2 + 1);
    uint16x8_t s1colsum0_l = vmlal_u8(vmovl_u8(vget_low_u8(s1r0)),
                                      vget_low_u8(s1r1), three_u8);
    uint16x8_t s1colsum0_h = vmlal_u8(vmovl_u8(vget_high_u8(s1r0)),
                                      vget_high_u8(s1r1), three_u8);
    uint16x8_t s1colsum1_l = vmlal_u8(vmovl_u8(vget_low_u8(s1r2)),
                                      vget_low_u8(s1r1), three_u8);
    uint16x8_t s1colsum1_h = vmlal_u8(vmovl_u8(vget_high_u8(s1r2)),
                                      vget_high_u8(s1r1), three_u8);
    /* Step 2: Blend the already-blended columns. */
    uint16x8_t output0_p1_l = vmlaq_u16(s1colsum0_l, s0colsum0_l, three_u16);
    uint16x8_t output0_p1_h = vmlaq_u16(s1colsum0_h, s0colsum0_h, three_u16);
    uint16x8_t output0_p2_l = vmlaq_u16(s0colsum0_l, s1colsum0_l, three_u16);
    uint16x8_t output0_p2_h = vmlaq_u16(s0colsum0_h, s1colsum0_h, three_u16);
    uint16x8_t output1_p1_l = vmlaq_u16(s1colsum1_l, s0colsum1_l, three_u16);
    uint16x8_t output1_p1_h = vmlaq_u16(s1colsum1_h, s0colsum1_h, three_u16);
    uint16x8_t output1_p2_l = vmlaq_u16(s0colsum1_l, s1colsum1_l, three_u16);
    uint16x8_t output1_p2_h = vmlaq_u16(s0colsum1_h, s1colsum1_h, three_u16);
    /* Add ordered dithering bias to odd pixel values. */
    output0_p1_l = vaddq_u16(output0_p1_l, seven_u16);
    output0_p1_h = vaddq_u16(output0_p1_h, seven_u16);
    output1_p1_l = vaddq_u16(output1_p1_l, seven_u16);
    output1_p1_h = vaddq_u16(output1_p1_h, seven_u16);
    /* Right-shift by 4 (divide by 16), narrow to 8-bit and combine. */
    uint8x16x2_t output_pixels0 = { vcombine_u8(vshrn_n_u16(output0_p1_l, 4),
                                                vshrn_n_u16(output0_p1_h, 4)),
                                    vcombine_u8(vrshrn_n_u16(output0_p2_l, 4),
                                                vrshrn_n_u16(output0_p2_h, 4))
                                  };
    uint8x16x2_t output_pixels1 = { vcombine_u8(vshrn_n_u16(output1_p1_l, 4),
                                                vshrn_n_u16(output1_p1_h, 4)),
                                    vcombine_u8(vrshrn_n_u16(output1_p2_l, 4),
                                                vrshrn_n_u16(output1_p2_h, 4))
                                  };
    /* Store pixel channel values to memory. */
    /* The minimum size of the output buffer for each row is 64 bytes => no */
    /* need to worry about buffer overflow here. See "Creation of 2-D sample */
    /* arrays" in jmemmgr.c for details. */
    vst2q_u8(outptr0 + 1, output_pixels0);
    vst2q_u8(outptr1 + 1, output_pixels1);

    /* The first pixel of the image shifted our loads and stores by one */
    /* byte. We have to re-align on a 32-byte boundary at some point before */
    /* the end of the row (we do it now on the 32/33 pixel boundary) to stay */
    /* within the bounds of the sample buffers without having to resort to a */
    /* slow scalar tail case for the last (downsampled_width % 16) samples. */
    /* See "Creation of 2-D sample arrays" in jmemmgr.c for details.*/
    for (unsigned colctr = 16; colctr < downsampled_width; colctr += 16) {
      /* Step 1: Blend samples vertically in columns s0 and s1. */
      /* Load and compute s0colsum0 and s0colsum1. */
      s0r0 = vld1q_u8(inptr0 + colctr - 1);
      s0r1 = vld1q_u8(inptr1 + colctr - 1);
      s0r2 = vld1q_u8(inptr2 + colctr - 1);
      s0colsum0_l = vmlal_u8(vmovl_u8(vget_low_u8(s0r0)),
                             vget_low_u8(s0r1), three_u8);
      s0colsum0_h = vmlal_u8(vmovl_u8(vget_high_u8(s0r0)),
                             vget_high_u8(s0r1), three_u8);
      s0colsum1_l = vmlal_u8(vmovl_u8(vget_low_u8(s0r2)),
                             vget_low_u8(s0r1), three_u8);
      s0colsum1_h = vmlal_u8(vmovl_u8(vget_high_u8(s0r2)),
                             vget_high_u8(s0r1), three_u8);
      /* Load and compute s1colsum0 and s1colsum1. */
      s1r0 = vld1q_u8(inptr0 + colctr);
      s1r1 = vld1q_u8(inptr1 + colctr);
      s1r2 = vld1q_u8(inptr2 + colctr);
      s1colsum0_l = vmlal_u8(vmovl_u8(vget_low_u8(s1r0)),
                             vget_low_u8(s1r1), three_u8);
      s1colsum0_h = vmlal_u8(vmovl_u8(vget_high_u8(s1r0)),
                             vget_high_u8(s1r1), three_u8);
      s1colsum1_l = vmlal_u8(vmovl_u8(vget_low_u8(s1r2)),
                             vget_low_u8(s1r1), three_u8);
      s1colsum1_h = vmlal_u8(vmovl_u8(vget_high_u8(s1r2)),
                             vget_high_u8(s1r1), three_u8);
      /* Step 2: Blend the already-blended columns. */
      output0_p1_l = vmlaq_u16(s1colsum0_l, s0colsum0_l, three_u16);
      output0_p1_h = vmlaq_u16(s1colsum0_h, s0colsum0_h, three_u16);
      output0_p2_l = vmlaq_u16(s0colsum0_l, s1colsum0_l, three_u16);
      output0_p2_h = vmlaq_u16(s0colsum0_h, s1colsum0_h, three_u16);
      output1_p1_l = vmlaq_u16(s1colsum1_l, s0colsum1_l, three_u16);
      output1_p1_h = vmlaq_u16(s1colsum1_h, s0colsum1_h, three_u16);
      output1_p2_l = vmlaq_u16(s0colsum1_l, s1colsum1_l, three_u16);
      output1_p2_h = vmlaq_u16(s0colsum1_h, s1colsum1_h, three_u16);
      /* Add ordered dithering bias to odd pixel values. */
      output0_p1_l = vaddq_u16(output0_p1_l, seven_u16);
      output0_p1_h = vaddq_u16(output0_p1_h, seven_u16);
      output1_p1_l = vaddq_u16(output1_p1_l, seven_u16);
      output1_p1_h = vaddq_u16(output1_p1_h, seven_u16);
      /* Right-shift by 4 (divide by 16), narrow to 8-bit and combine. */
      output_pixels0.val[0] = vcombine_u8(vshrn_n_u16(output0_p1_l, 4),
                                          vshrn_n_u16(output0_p1_h, 4));
      output_pixels0.val[1] = vcombine_u8(vrshrn_n_u16(output0_p2_l, 4),
                                          vrshrn_n_u16(output0_p2_h, 4));
      output_pixels1.val[0] = vcombine_u8(vshrn_n_u16(output1_p1_l, 4),
                                          vshrn_n_u16(output1_p1_h, 4));
      output_pixels1.val[1] = vcombine_u8(vrshrn_n_u16(output1_p2_l, 4),
                                          vrshrn_n_u16(output1_p2_h, 4));
      /* Store pixel channel values to memory. */
      vst2q_u8(outptr0 + 2 * colctr - 1, output_pixels0);
      vst2q_u8(outptr1 + 2 * colctr - 1, output_pixels1);
    }

    /* Case 2: last pixel channel value in this row of the original image. */
    int s1colsum0 = GETJSAMPLE(inptr1[downsampled_width - 1]) * 3 +
                    GETJSAMPLE(inptr0[downsampled_width - 1]);
    outptr0[2 * downsampled_width - 1] = (JSAMPLE)((s1colsum0 * 4 + 7) >> 4);
    int s1colsum1 = GETJSAMPLE(inptr1[downsampled_width - 1]) * 3 +
                    GETJSAMPLE(inptr2[downsampled_width - 1]);
    outptr1[2 * downsampled_width - 1] = (JSAMPLE)((s1colsum1 * 4 + 7) >> 4);
    inrow++;
  }
}


/*
 * The diagram below shows a grid-window of samples (luma or chroma) produced
 * by h2v1 downsampling; which has been subsequently rotated 90 degrees. (The
 * usual use of h1v2 upsampling is upsampling rotated or transposed h2v1
 * downsampled images.)
 *
 *                  s0        s1
 *             +---------+---------+
 *             |    p0   |    p1   |
 *     r0      |         |         |
 *             |    p2   |    p3   |
 *             +---------+---------+
 *             |    p4   |    p5   |
 *     r1      |         |         |
 *             |    p6   |    p7   |
 *             +---------+---------+
 *             |    p8   |    p9   |
 *     r2      |         |         |
 *             |    p10  |    p11  |
 *             +---------+---------+
 *
 * Every sample contains two of the original pixel channel values. The pixels'
 * channel values are centred at positions p0, p1, p2,..., p11 above. For a
 * given grid-window position, r1 is always used to denote the row of samples
 * containing the pixel channel values we are computing. For the top row of
 * pixel channel values in r1 (p4 and p5), the nearest neighbouring samples are
 * in the row above - denoted by r0. Likewise, for the bottom row of pixels in
 * r1 (p6 and p7), the nearest neighbouring samples are in the row below -
 * denoted by r2.
 *
 * To compute the pixel channel values of the original image, we proportionally
 * blend the adjacent samples in each column.
 *
 * For example, the pixel channel value centred at p4 would be computed as
 * follows:
 *     3/4 * s0r1 + 1/4 * s0r0
 * while the pixel channel value centred at p6 would be:
 *     3/4 * s0r1 + 1/4 * s0r2
 */

void jsimd_h1v2_fancy_upsample_neon(int max_v_samp_factor,
                                    JDIMENSION downsampled_width,
                                    JSAMPARRAY input_data,
                                    JSAMPARRAY *output_data_ptr)
{
  JSAMPARRAY output_data = *output_data_ptr;
  JSAMPROW inptr0, inptr1, inptr2, outptr0, outptr1;
  int inrow, outrow;
  /* Setup constants. */
  const uint16x8_t one_u16 = vdupq_n_u16(1);
  const uint8x8_t three_u8 = vdup_n_u8(3);

  inrow = outrow = 0;
  while (outrow < max_v_samp_factor) {
    inptr0 = input_data[inrow - 1];
    inptr1 = input_data[inrow];
    inptr2 = input_data[inrow + 1];
    /* Suffixes 0 and 1 denote the top and bottom rows of output pixels */
    /* respectively. */
    outptr0 = output_data[outrow++];
    outptr1 = output_data[outrow++];
    inrow++;

    /* The size of the input and output buffers is always a multiple of 32 */
    /* bytes => no need to worry about buffer overflow when reading/writing */
    /* memory. See "Creation of 2-D sample arrays" in jmemmgr.c for details. */
    for (unsigned colctr = 0; colctr < downsampled_width; colctr += 16) {
      /* Load samples. */
      uint8x16_t r0 = vld1q_u8(inptr0 + colctr);
      uint8x16_t r1 = vld1q_u8(inptr1 + colctr);
      uint8x16_t r2 = vld1q_u8(inptr2 + colctr);
      /* Blend samples vertically. */
      uint16x8_t colsum0_l = vmlal_u8(vmovl_u8(vget_low_u8(r0)),
                                      vget_low_u8(r1), three_u8);
      uint16x8_t colsum0_h = vmlal_u8(vmovl_u8(vget_high_u8(r0)),
                                      vget_high_u8(r1), three_u8);
      uint16x8_t colsum1_l = vmlal_u8(vmovl_u8(vget_low_u8(r2)),
                                      vget_low_u8(r1), three_u8);
      uint16x8_t colsum1_h = vmlal_u8(vmovl_u8(vget_high_u8(r2)),
                                      vget_high_u8(r1), three_u8);
      /* Add ordered dithering bias to pixel values in even output rows. */
      colsum0_l = vaddq_u16(colsum0_l, one_u16);
      colsum0_h = vaddq_u16(colsum0_h, one_u16);
      /* Right-shift by 2 (divide by 4), narrow to 8-bit and combine. */
      uint8x16_t output_pixels0 = vcombine_u8(vshrn_n_u16(colsum0_l, 2),
                                              vshrn_n_u16(colsum0_h, 2));
      uint8x16_t output_pixels1 = vcombine_u8(vrshrn_n_u16(colsum1_l, 2),
                                              vrshrn_n_u16(colsum1_h, 2));
      /* Store pixel channel values to memory. */
      vst1q_u8(outptr0 + colctr, output_pixels0);
      vst1q_u8(outptr1 + colctr, output_pixels1);
    }
  }
}
