#ifndef RVOS_QOI_H
#define RVOS_QOI_H

#include "types.h"

/* Decode a QOI image into packed 0xAARRGGBB pixels. */
int qoi_decode(const u8 *data, u32 length, u32 *out, u32 max_pixels,
               u32 *out_width, u32 *out_height);

#endif
