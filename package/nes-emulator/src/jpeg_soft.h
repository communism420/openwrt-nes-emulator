#ifndef JPEG_SOFT_H
#define JPEG_SOFT_H

#include <stddef.h>
#include <stdint.h>

/*
 * Baseline software JPEG encoder (CPU only).
 * Input: native-endian RGB565 words, tightly packed row-major.
 * Output: complete JPEG bitstream written to out (out_cap bytes).
 * quality: 1..100 (typical 50–85 for NES stream).
 * Returns bytes written, or 0 on failure.
 */
size_t jpeg_encode_rgb565(const uint16_t *rgb565, unsigned width, unsigned height,
	int quality, uint8_t *out, size_t out_cap);

#endif
