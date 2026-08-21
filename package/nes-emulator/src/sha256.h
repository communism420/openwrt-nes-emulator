#ifndef NES_SHA256_H
#define NES_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define NES_SHA256_BYTES 32

struct nes_sha256 {
	uint32_t state[8];
	uint64_t length;
	uint8_t block[64];
	size_t used;
};

void nes_sha256_init(struct nes_sha256 *context);
void nes_sha256_update(struct nes_sha256 *context, const void *data,
	size_t length);
void nes_sha256_final(struct nes_sha256 *context,
	uint8_t digest[NES_SHA256_BYTES]);
void nes_sha256_digest(const void *data, size_t length,
	uint8_t digest[NES_SHA256_BYTES]);

#endif
