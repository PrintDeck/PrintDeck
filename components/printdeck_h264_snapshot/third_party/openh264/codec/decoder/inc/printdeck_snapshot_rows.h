// PrintDeck's IDR-only decoder keeps two macroblock rows instead of a picture.
#pragma once

inline int PrintDeckPictureRow(int logical_row) {
#ifdef WELS_IDR_ROWS
  return 1;
#else
  return logical_row;
#endif
}

#ifdef WELS_IDR_ROWS
#ifndef WELS_IDR_ONLY
#error WELS_IDR_ROWS requires the single-picture IDR-only decoder
#endif
// Called synchronously after deblocking. Pointers expire at the next row.
// Return false to cancel; a partial thumbnail must never be published.
extern "C" bool printdeck_h264_row(int row, int width, int height,
                                  const unsigned char* y, const unsigned char* u,
                                  const unsigned char* v, int y_stride, int c_stride);
#endif
