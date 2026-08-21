/*
 * Software baseline JPEG encoder (CPU only, no GPU).
 * Color YCbCr 4:4:4, for streaming NES frames over WebSocket.
 */
#include "jpeg_soft.h"

#include <limits.h>
#include <string.h>

static const unsigned char STD_DC_LUMINANCE_NRCODES[17] = {
	0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0
};
static const unsigned char STD_DC_LUMINANCE_VALUES[12] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
};
static const unsigned char STD_AC_LUMINANCE_NRCODES[17] = {
	0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d
};
static const unsigned char STD_AC_LUMINANCE_VALUES[] = {
	0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
	0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
	0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
	0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
	0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
	0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
	0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
	0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
	0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
	0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
	0xf9, 0xfa
};
static const unsigned char STD_DC_CHROMINANCE_NRCODES[17] = {
	0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0
};
static const unsigned char STD_DC_CHROMINANCE_VALUES[12] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
};
static const unsigned char STD_AC_CHROMINANCE_NRCODES[17] = {
	0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77
};
static const unsigned char STD_AC_CHROMINANCE_VALUES[] = {
	0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
	0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
	0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
	0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
	0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
	0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
	0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
	0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
	0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
	0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
	0xf9, 0xfa
};

static const unsigned char ZIGZAG[64] = {
	0, 1, 5, 6, 14, 15, 27, 28,
	2, 4, 7, 13, 16, 26, 29, 42,
	3, 8, 12, 17, 25, 30, 41, 43,
	9, 11, 18, 24, 31, 40, 44, 53,
	10, 19, 23, 32, 39, 45, 52, 54,
	20, 22, 33, 38, 46, 51, 55, 60,
	21, 34, 37, 47, 50, 56, 59, 61,
	35, 36, 48, 49, 57, 58, 62, 63
};

static const float AAN_SCALE[8] = {
	1.0f, 1.387039845f, 1.306562965f, 1.175875602f,
	1.0f, 0.785694958f, 0.541196100f, 0.275899379f
};

static const unsigned char STD_LUM_QT[64] = {
	16, 11, 10, 16, 24, 40, 51, 61,
	12, 12, 14, 19, 26, 58, 60, 55,
	14, 13, 16, 24, 40, 57, 69, 56,
	14, 17, 22, 29, 51, 87, 80, 62,
	18, 22, 37, 56, 68, 109, 103, 77,
	24, 35, 55, 64, 81, 104, 113, 92,
	49, 64, 78, 87, 103, 121, 120, 101,
	72, 92, 95, 98, 112, 100, 103, 99
};

static const unsigned char STD_CHR_QT[64] = {
	17, 18, 24, 47, 99, 99, 99, 99,
	18, 21, 26, 66, 99, 99, 99, 99,
	24, 26, 56, 99, 99, 99, 99, 99,
	47, 66, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99
};

struct jbit {
	uint8_t *out;
	size_t cap;
	size_t len;
	uint32_t bitbuf;
	int bitc;
	int err;
};

struct huff {
	unsigned code[256];
	uint8_t size[256];
};

static void jb_put(struct jbit *b, uint8_t v)
{
	if (b->err)
		return;
	if (b->len >= b->cap) {
		b->err = 1;
		return;
	}
	b->out[b->len++] = v;
}

static void jb_bits(struct jbit *b, unsigned code, int size)
{
	if (size <= 0 || b->err)
		return;
	b->bitbuf = (b->bitbuf << size) | (code & ((1u << size) - 1u));
	b->bitc += size;
	while (b->bitc >= 8) {
		uint8_t c = (uint8_t)((b->bitbuf >> (b->bitc - 8)) & 0xff);
		jb_put(b, c);
		if (c == 0xff)
			jb_put(b, 0x00);
		b->bitc -= 8;
	}
}

static void jb_flush(struct jbit *b)
{
	if (b->bitc > 0) {
		int padding = 8 - b->bitc;
		jb_bits(b, (1u << padding) - 1u, padding);
	}
	b->bitc = 0;
	b->bitbuf = 0;
}

static void build_huff(struct huff *h, const unsigned char *nrcodes,
	const unsigned char *values)
{
	unsigned code = 0;
	int i, j, k = 0;
	memset(h, 0, sizeof(*h));
	for (i = 1; i <= 16; i++) {
		for (j = 1; j <= nrcodes[i]; j++) {
			unsigned char v = values[k++];
			h->code[v] = code;
			h->size[v] = (uint8_t)i;
			code++;
		}
		code <<= 1;
	}
}

static int category(int v)
{
	int c = 0;
	int a = v < 0 ? -v : v;
	while (a) {
		a >>= 1;
		c++;
	}
	return c;
}

static void write_marker(struct jbit *b, uint8_t m)
{
	jb_put(b, 0xff);
	jb_put(b, m);
}

static void write_dqt(struct jbit *b, const uint8_t q[64], int id)
{
	uint8_t zigzag_order[64];
	int i;

	/*
	 * ZIGZAG maps a natural-order coefficient index to its position in
	 * the JPEG zigzag stream. Build the inverse ordering for DQT output.
	 */
	for (i = 0; i < 64; i++)
		zigzag_order[ZIGZAG[i]] = q[i];
	write_marker(b, 0xDB);
	jb_put(b, 0x00);
	jb_put(b, 0x43);
	jb_put(b, (uint8_t)id);
	for (i = 0; i < 64; i++)
		jb_put(b, zigzag_order[i]);
}

static void write_dht(struct jbit *b, int class_id, int table_id,
	const unsigned char *nrcodes, const unsigned char *values, int nvalues)
{
	int i, len = 19 + nvalues;
	write_marker(b, 0xC4);
	jb_put(b, (uint8_t)(len >> 8));
	jb_put(b, (uint8_t)(len & 0xff));
	jb_put(b, (uint8_t)((class_id << 4) | table_id));
	for (i = 1; i <= 16; i++)
		jb_put(b, nrcodes[i]);
	for (i = 0; i < nvalues; i++)
		jb_put(b, values[i]);
}

static void fdct(float *data)
{
	int i;
	for (i = 0; i < 8; i++) {
		float *d = data + i * 8;
		float tmp0 = d[0] + d[7];
		float tmp7 = d[0] - d[7];
		float tmp1 = d[1] + d[6];
		float tmp6 = d[1] - d[6];
		float tmp2 = d[2] + d[5];
		float tmp5 = d[2] - d[5];
		float tmp3 = d[3] + d[4];
		float tmp4 = d[3] - d[4];
		float tmp10 = tmp0 + tmp3;
		float tmp13 = tmp0 - tmp3;
		float tmp11 = tmp1 + tmp2;
		float tmp12 = tmp1 - tmp2;
		float z1, z2, z3, z4, z5, z11, z13;
		d[0] = tmp10 + tmp11;
		d[4] = tmp10 - tmp11;
		z1 = (tmp12 + tmp13) * 0.707106781f;
		d[2] = tmp13 + z1;
		d[6] = tmp13 - z1;
		tmp10 = tmp4 + tmp5;
		tmp11 = tmp5 + tmp6;
		tmp12 = tmp6 + tmp7;
		z5 = (tmp10 - tmp12) * 0.382683433f;
		z2 = tmp10 * 0.541196100f + z5;
		z4 = tmp12 * 1.306562965f + z5;
		z3 = tmp11 * 0.707106781f;
		z11 = tmp7 + z3;
		z13 = tmp7 - z3;
		d[5] = z13 + z2;
		d[3] = z13 - z2;
		d[1] = z11 + z4;
		d[7] = z11 - z4;
	}
	for (i = 0; i < 8; i++) {
		float *d = data + i;
		float tmp0 = d[0] + d[56];
		float tmp7 = d[0] - d[56];
		float tmp1 = d[8] + d[48];
		float tmp6 = d[8] - d[48];
		float tmp2 = d[16] + d[40];
		float tmp5 = d[16] - d[40];
		float tmp3 = d[24] + d[32];
		float tmp4 = d[24] - d[32];
		float tmp10 = tmp0 + tmp3;
		float tmp13 = tmp0 - tmp3;
		float tmp11 = tmp1 + tmp2;
		float tmp12 = tmp1 - tmp2;
		float z1, z2, z3, z4, z5, z11, z13;
		d[0] = tmp10 + tmp11;
		d[32] = tmp10 - tmp11;
		z1 = (tmp12 + tmp13) * 0.707106781f;
		d[16] = tmp13 + z1;
		d[48] = tmp13 - z1;
		tmp10 = tmp4 + tmp5;
		tmp11 = tmp5 + tmp6;
		tmp12 = tmp6 + tmp7;
		z5 = (tmp10 - tmp12) * 0.382683433f;
		z2 = tmp10 * 0.541196100f + z5;
		z4 = tmp12 * 1.306562965f + z5;
		z3 = tmp11 * 0.707106781f;
		z11 = tmp7 + z3;
		z13 = tmp7 - z3;
		d[40] = z13 + z2;
		d[24] = z13 - z2;
		d[8] = z11 + z4;
		d[56] = z11 - z4;
	}
}

static void encode_du(struct jbit *b, int *du, int *dc, const float *fdtbl,
	const struct huff *dc_h, const struct huff *ac_h)
{
	float cdct[64];
	int i, qdu[64];
	int diff, last = 0;

	for (i = 0; i < 64; i++)
		cdct[i] = (float)du[i];
	fdct(cdct);
	for (i = 0; i < 64; i++) {
		float v = cdct[i] * fdtbl[i];
		qdu[ZIGZAG[i]] = (int)(v < 0 ? v - 0.5f : v + 0.5f);
	}

	diff = qdu[0] - *dc;
	*dc = qdu[0];
	if (diff == 0) {
		jb_bits(b, dc_h->code[0], dc_h->size[0]);
	} else {
		int cat = category(diff);
		int mag = diff;
		if (mag < 0)
			mag = mag + ((1 << cat) - 1);
		jb_bits(b, dc_h->code[cat], dc_h->size[cat]);
		jb_bits(b, (unsigned)mag, cat);
	}

	for (i = 1; i < 64; i++) {
		if (qdu[i] == 0) {
			last++;
			continue;
		}
		while (last > 15) {
			jb_bits(b, ac_h->code[0xF0], ac_h->size[0xF0]);
			last -= 16;
		}
		{
			int cat = category(qdu[i]);
			int mag = qdu[i];
			int rs = (last << 4) | cat;
			if (mag < 0)
				mag = mag + ((1 << cat) - 1);
			jb_bits(b, ac_h->code[rs], ac_h->size[rs]);
			jb_bits(b, (unsigned)mag, cat);
			last = 0;
		}
	}
	if (last > 0)
		jb_bits(b, ac_h->code[0], ac_h->size[0]);
}

static void scale_qt(const unsigned char *src, uint8_t *dst, int quality)
{
	int i, s = quality < 50 ? 5000 / quality : 200 - quality * 2;
	for (i = 0; i < 64; i++) {
		int v = (src[i] * s + 50) / 100;
		if (v < 1)
			v = 1;
		if (v > 255)
			v = 255;
		dst[i] = (uint8_t)v;
	}
}

static void make_fdtbl(const uint8_t *qt, float *fdtbl)
{
	int i;
	for (i = 0; i < 64; i++) {
		int row = i >> 3, col = i & 7;
		/* qt[] natural order; fdtbl multiplies natural-order DCT coeffs */
		fdtbl[i] = 1.0f / ((float)qt[i] * AAN_SCALE[row] * AAN_SCALE[col] * 8.0f);
	}
}

static void rgb565_to_ycbcr(uint16_t p, int *Y, int *Cb, int *Cr)
{
	int r = ((p >> 11) & 31) * 255 / 31;
	int g = ((p >> 5) & 63) * 255 / 63;
	int b = (p & 31) * 255 / 31;

	/* Full-range JFIF YCbCr: black maps to Y=0 and white to Y=255. */
	int cb_delta = -43 * r - 85 * g + 128 * b;
	int cr_delta = 128 * r - 107 * g - 21 * b;

	*Y = (77 * r + 150 * g + 29 * b + 128) / 256;
	*Cb = 128 + (cb_delta >= 0 ?
		(cb_delta + 128) / 256 : -((-cb_delta + 128) / 256));
	*Cr = 128 + (cr_delta >= 0 ?
		(cr_delta + 128) / 256 : -((-cr_delta + 128) / 256));
	if (*Y < 0)
		*Y = 0;
	if (*Y > 255)
		*Y = 255;
	if (*Cb < 0)
		*Cb = 0;
	if (*Cb > 255)
		*Cb = 255;
	if (*Cr < 0)
		*Cr = 0;
	if (*Cr > 255)
		*Cr = 255;
}

size_t jpeg_encode_rgb565(const uint16_t *rgb565, unsigned width, unsigned height,
	int quality, uint8_t *out, size_t out_cap)
{
	struct jbit b;
	struct huff dc_y, ac_y, dc_c, ac_c;
	uint8_t qt_y[64], qt_c[64];
	float fdtbl_y[64], fdtbl_c[64];
	int dcY = 0, dcCb = 0, dcCr = 0;
	unsigned x, y;

	if (!rgb565 || !out || width < 8 || height < 8 ||
	    width > UINT16_MAX || height > UINT16_MAX || out_cap < 512)
		return 0;
	if (quality < 1)
		quality = 1;
	if (quality > 100)
		quality = 100;

	scale_qt(STD_LUM_QT, qt_y, quality);
	scale_qt(STD_CHR_QT, qt_c, quality);
	make_fdtbl(qt_y, fdtbl_y);
	make_fdtbl(qt_c, fdtbl_c);

	memset(&b, 0, sizeof(b));
	b.out = out;
	b.cap = out_cap;

	build_huff(&dc_y, STD_DC_LUMINANCE_NRCODES, STD_DC_LUMINANCE_VALUES);
	build_huff(&ac_y, STD_AC_LUMINANCE_NRCODES, STD_AC_LUMINANCE_VALUES);
	build_huff(&dc_c, STD_DC_CHROMINANCE_NRCODES, STD_DC_CHROMINANCE_VALUES);
	build_huff(&ac_c, STD_AC_CHROMINANCE_NRCODES, STD_AC_CHROMINANCE_VALUES);

	write_marker(&b, 0xD8);
	/* APP0 */
	write_marker(&b, 0xE0);
	jb_put(&b, 0x00); jb_put(&b, 0x10);
	jb_put(&b, 'J'); jb_put(&b, 'F'); jb_put(&b, 'I'); jb_put(&b, 'F'); jb_put(&b, 0);
	jb_put(&b, 1); jb_put(&b, 1); jb_put(&b, 0);
	jb_put(&b, 0); jb_put(&b, 1); jb_put(&b, 0); jb_put(&b, 1);
	jb_put(&b, 0); jb_put(&b, 0);
	write_dqt(&b, qt_y, 0);
	write_dqt(&b, qt_c, 1);
	/* SOF0 color 4:4:4 */
	write_marker(&b, 0xC0);
	jb_put(&b, 0x00); jb_put(&b, 0x11);
	jb_put(&b, 8);
	jb_put(&b, (uint8_t)(height >> 8));
	jb_put(&b, (uint8_t)(height & 0xff));
	jb_put(&b, (uint8_t)(width >> 8));
	jb_put(&b, (uint8_t)(width & 0xff));
	jb_put(&b, 3);
	jb_put(&b, 1); jb_put(&b, 0x11); jb_put(&b, 0);
	jb_put(&b, 2); jb_put(&b, 0x11); jb_put(&b, 1);
	jb_put(&b, 3); jb_put(&b, 0x11); jb_put(&b, 1);
	write_dht(&b, 0, 0, STD_DC_LUMINANCE_NRCODES, STD_DC_LUMINANCE_VALUES, 12);
	write_dht(&b, 1, 0, STD_AC_LUMINANCE_NRCODES, STD_AC_LUMINANCE_VALUES, 162);
	write_dht(&b, 0, 1, STD_DC_CHROMINANCE_NRCODES, STD_DC_CHROMINANCE_VALUES, 12);
	write_dht(&b, 1, 1, STD_AC_CHROMINANCE_NRCODES, STD_AC_CHROMINANCE_VALUES, 162);
	/* SOS */
	write_marker(&b, 0xDA);
	jb_put(&b, 0x00); jb_put(&b, 0x0C);
	jb_put(&b, 3);
	jb_put(&b, 1); jb_put(&b, 0x00);
	jb_put(&b, 2); jb_put(&b, 0x11);
	jb_put(&b, 3); jb_put(&b, 0x11);
	jb_put(&b, 0); jb_put(&b, 63); jb_put(&b, 0);

	for (y = 0; y < height; y += 8) {
		for (x = 0; x < width; x += 8) {
			int duY[64], duCb[64], duCr[64];
			unsigned by, bx;
			for (by = 0; by < 8; by++) {
				unsigned sy = y + by;
				if (sy >= height)
					sy = height - 1;
				for (bx = 0; bx < 8; bx++) {
					unsigned sx = x + bx;
					int Yv, Cb, Cr;
					uint16_t p;
					if (sx >= width)
						sx = width - 1;
					p = rgb565[(size_t)sy * width + sx];
					rgb565_to_ycbcr(p, &Yv, &Cb, &Cr);
					duY[by * 8 + bx] = Yv - 128;
					duCb[by * 8 + bx] = Cb - 128;
					duCr[by * 8 + bx] = Cr - 128;
				}
			}
			encode_du(&b, duY, &dcY, fdtbl_y, &dc_y, &ac_y);
			encode_du(&b, duCb, &dcCb, fdtbl_c, &dc_c, &ac_c);
			encode_du(&b, duCr, &dcCr, fdtbl_c, &dc_c, &ac_c);
			if (b.err)
				return 0;
		}
	}

	jb_flush(&b);
	write_marker(&b, 0xD9);
	if (b.err)
		return 0;
	return b.len;
}
