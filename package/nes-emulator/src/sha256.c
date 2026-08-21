/* SPDX-License-Identifier: MIT */
#include "sha256.h"

#include <string.h>

static const uint32_t round_constants[64] = {
	UINT32_C(0x428a2f98), UINT32_C(0x71374491), UINT32_C(0xb5c0fbcf),
	UINT32_C(0xe9b5dba5), UINT32_C(0x3956c25b), UINT32_C(0x59f111f1),
	UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5), UINT32_C(0xd807aa98),
	UINT32_C(0x12835b01), UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
	UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe), UINT32_C(0x9bdc06a7),
	UINT32_C(0xc19bf174), UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786),
	UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc), UINT32_C(0x2de92c6f),
	UINT32_C(0x4a7484aa), UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
	UINT32_C(0x983e5152), UINT32_C(0xa831c66d), UINT32_C(0xb00327c8),
	UINT32_C(0xbf597fc7), UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147),
	UINT32_C(0x06ca6351), UINT32_C(0x14292967), UINT32_C(0x27b70a85),
	UINT32_C(0x2e1b2138), UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
	UINT32_C(0x650a7354), UINT32_C(0x766a0abb), UINT32_C(0x81c2c92e),
	UINT32_C(0x92722c85), UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b),
	UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3), UINT32_C(0xd192e819),
	UINT32_C(0xd6990624), UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
	UINT32_C(0x19a4c116), UINT32_C(0x1e376c08), UINT32_C(0x2748774c),
	UINT32_C(0x34b0bcb5), UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a),
	UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3), UINT32_C(0x748f82ee),
	UINT32_C(0x78a5636f), UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
	UINT32_C(0x90befffa), UINT32_C(0xa4506ceb), UINT32_C(0xbef9a3f7),
	UINT32_C(0xc67178f2)
};

static uint32_t rotate_right(uint32_t value, unsigned count)
{
	return (value >> count) | (value << (32u - count));
}

static uint32_t load_be32(const uint8_t *bytes)
{
	return ((uint32_t)bytes[0] << 24) |
	       ((uint32_t)bytes[1] << 16) |
	       ((uint32_t)bytes[2] << 8) |
	       (uint32_t)bytes[3];
}

static void store_be32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)(value >> 24);
	bytes[1] = (uint8_t)(value >> 16);
	bytes[2] = (uint8_t)(value >> 8);
	bytes[3] = (uint8_t)value;
}

static void transform(struct nes_sha256 *context, const uint8_t block[64])
{
	uint32_t words[64];
	uint32_t a, b, c, d, e, f, g, h;
	unsigned i;

	for (i = 0; i < 16; i++)
		words[i] = load_be32(block + i * 4);
	for (i = 16; i < 64; i++) {
		uint32_t s0 = rotate_right(words[i - 15], 7) ^
			rotate_right(words[i - 15], 18) ^ (words[i - 15] >> 3);
		uint32_t s1 = rotate_right(words[i - 2], 17) ^
			rotate_right(words[i - 2], 19) ^ (words[i - 2] >> 10);

		words[i] = words[i - 16] + s0 + words[i - 7] + s1;
	}
	a = context->state[0];
	b = context->state[1];
	c = context->state[2];
	d = context->state[3];
	e = context->state[4];
	f = context->state[5];
	g = context->state[6];
	h = context->state[7];
	for (i = 0; i < 64; i++) {
		uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^
			rotate_right(e, 25);
		uint32_t choose = (e & f) ^ (~e & g);
		uint32_t temporary1 = h + sum1 + choose +
			round_constants[i] + words[i];
		uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^
			rotate_right(a, 22);
		uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
		uint32_t temporary2 = sum0 + majority;

		h = g;
		g = f;
		f = e;
		e = d + temporary1;
		d = c;
		c = b;
		b = a;
		a = temporary1 + temporary2;
	}
	context->state[0] += a;
	context->state[1] += b;
	context->state[2] += c;
	context->state[3] += d;
	context->state[4] += e;
	context->state[5] += f;
	context->state[6] += g;
	context->state[7] += h;
}

void nes_sha256_init(struct nes_sha256 *context)
{
	static const uint32_t initial_state[8] = {
		UINT32_C(0x6a09e667), UINT32_C(0xbb67ae85),
		UINT32_C(0x3c6ef372), UINT32_C(0xa54ff53a),
		UINT32_C(0x510e527f), UINT32_C(0x9b05688c),
		UINT32_C(0x1f83d9ab), UINT32_C(0x5be0cd19)
	};

	memset(context, 0, sizeof(*context));
	memcpy(context->state, initial_state, sizeof(initial_state));
}

void nes_sha256_update(struct nes_sha256 *context, const void *data,
	size_t length)
{
	const uint8_t *bytes = data;

	if (!length)
		return;
	context->length += (uint64_t)length;
	while (length > 0) {
		size_t available = sizeof(context->block) - context->used;
		size_t take = length < available ? length : available;

		memcpy(context->block + context->used, bytes, take);
		context->used += take;
		bytes += take;
		length -= take;
		if (context->used == sizeof(context->block)) {
			transform(context, context->block);
			context->used = 0;
		}
	}
}

void nes_sha256_final(struct nes_sha256 *context,
	uint8_t digest[NES_SHA256_BYTES])
{
	uint64_t bit_length = context->length * UINT64_C(8);
	unsigned i;

	context->block[context->used++] = 0x80;
	if (context->used > 56) {
		memset(context->block + context->used, 0,
			sizeof(context->block) - context->used);
		transform(context, context->block);
		context->used = 0;
	}
	memset(context->block + context->used, 0, 56 - context->used);
	for (i = 0; i < 8; i++)
		context->block[56 + i] = (uint8_t)(bit_length >> (56 - i * 8));
	transform(context, context->block);
	for (i = 0; i < 8; i++)
		store_be32(digest + i * 4, context->state[i]);
	memset(context, 0, sizeof(*context));
}

void nes_sha256_digest(const void *data, size_t length,
	uint8_t digest[NES_SHA256_BYTES])
{
	struct nes_sha256 context;

	nes_sha256_init(&context);
	nes_sha256_update(&context, data, length);
	nes_sha256_final(&context, digest);
}
