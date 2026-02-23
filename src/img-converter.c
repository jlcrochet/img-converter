/*
 * img-converter - Convert between image formats
 * Supports: PNG, JPEG, BMP, QOI, plus optional TIFF, WebP, AVIF, HEIF, JXL
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdbool.h>
#include <getopt.h>
#include <limits.h>
#include <sys/stat.h>
#include <png.h>
#include <jpeglib.h>
#ifdef HAVE_TIFF
#include <tiffio.h>
#endif
#ifdef HAVE_WEBP
#include <webp/decode.h>
#include <webp/encode.h>
#endif
#ifdef HAVE_AVIF
#include <avif/avif.h>
#endif
#ifdef HAVE_HEIF
#include <libheif/heif.h>
#endif
#ifdef HAVE_JXL
#include <jxl/decode.h>
#include <jxl/encode.h>
#include <jxl/resizable_parallel_runner.h>
#endif

#include "lib/stdio_helpers.h"
#include "lib/checked_arith.h"

// QOI format implementation (inline, no library needed)
#define QOI_OP_INDEX  0x00
#define QOI_OP_DIFF   0x40
#define QOI_OP_LUMA   0x80
#define QOI_OP_RUN    0xc0
#define QOI_OP_RGB    0xfe
#define QOI_OP_RGBA   0xff
#define QOI_MASK_2    0xc0
#define QOI_MAGIC     0x716f6966  // "qoif"
#define QOI_HEADER_SIZE 14
#define QOI_END_MARKER_SIZE 8

static inline uint32_t qoi_read32be(const uint8_t *p) {
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline void qoi_write32be(uint8_t *p, uint32_t v) {
	p[0] = (v >> 24) & 0xff;
	p[1] = (v >> 16) & 0xff;
	p[2] = (v >> 8) & 0xff;
	p[3] = v & 0xff;
}

static inline int qoi_hash(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	return (r * 3 + g * 5 + b * 7 + a * 11) % 64;
}

// ============================================================================
// Image data
// ============================================================================

// Read entire file into malloc'd buffer, returns NULL on failure
#if defined(HAVE_WEBP) || defined(HAVE_AVIF) || defined(HAVE_JXL)
static uint8_t *read_file(const char *path, size_t *out_size)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;

	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	long end = ftell(f);
	if (end < 0) {
		fclose(f);
		return NULL;
	}
	if ((unsigned long)end > SIZE_MAX) {
		fclose(f);
		return NULL;
	}
	if (fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	size_t size = (size_t)end;
	if (max_bytes != 0 && size > max_bytes) {
		fclose(f);
		errno = EFBIG;
		return NULL;
	}

	uint8_t *data = malloc(size ? size : 1);
	if (!data) {
		fclose(f);
		return NULL;
	}

	if (size > 0 && READ_FILE(data, size, f) != size) {
		free(data);
		fclose(f);
		return NULL;
	}

	fclose(f);
	*out_size = size;
	return data;
}
#endif

static bool read_bytes(FILE *f, uint8_t *buf, size_t len)
{
	if (len == 0)
		return true;
	return READ_FILE(buf, len, f) == len;
}

static bool read_byte(FILE *f, uint8_t *out)
{
	int c = GETC_FILE(f);
	if (c == EOF)
		return false;
	*out = (uint8_t)c;
	return true;
}

struct image {
	uint8_t *pixels;    // RGB or RGBA
	int width;
	int height;
	int channels;       // 3 = RGB, 4 = RGBA
};

static size_t max_pixels = 100000000;  // 0 = unlimited
static size_t max_bytes = 268435456;   // 0 = unlimited

static bool image_validate_dims(const struct image *img)
{
	return img->width > 0 && img->height > 0 && (img->channels == 3 || img->channels == 4);
}

static bool image_check_max_pixels(int width, int height)
{
	size_t pixel_count;
	if (!checked_mul_size((size_t)width, (size_t)height, &pixel_count))
		return false;
	return max_pixels == 0 || pixel_count <= max_pixels;
}

static bool image_alloc_pixels(struct image *img, size_t rowbytes)
{
	if (!image_validate_dims(img) || rowbytes == 0)
		return false;
	if (!image_check_max_pixels(img->width, img->height))
		return false;
	size_t total;
	if (!checked_mul_size(rowbytes, (size_t)img->height, &total))
		return false;
	img->pixels = malloc(total);
	return img->pixels != NULL;
}

// ============================================================================
// PNG
// ============================================================================

static bool png_read(const char *path, struct image *img)
{
	FILE *f = fopen(path, "rb");
	if (!f) return false;

	png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!png) {
		fclose(f);
		return false;
	}

	png_infop info = png_create_info_struct(png);
	if (!info) {
		png_destroy_read_struct(&png, NULL, NULL);
		fclose(f);
		return false;
	}

	if (setjmp(png_jmpbuf(png))) {
		png_destroy_read_struct(&png, &info, NULL);
		fclose(f);
		return false;
	}

	png_init_io(png, f);
	png_read_info(png, info);

	png_uint_32 w = png_get_image_width(png, info);
	png_uint_32 h = png_get_image_height(png, info);
	if (w == 0 || h == 0 || w > INT_MAX || h > INT_MAX) {
		png_destroy_read_struct(&png, &info, NULL);
		fclose(f);
		return false;
	}
	img->width = (int)w;
	img->height = (int)h;
	png_byte color_type = png_get_color_type(png, info);
	png_byte bit_depth = png_get_bit_depth(png, info);

	// Convert to 8-bit RGBA
	if (bit_depth == 16)
		png_set_strip_16(png);
	if (color_type == PNG_COLOR_TYPE_PALETTE)
		png_set_palette_to_rgb(png);
	if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
		png_set_expand_gray_1_2_4_to_8(png);
	if (png_get_valid(png, info, PNG_INFO_tRNS))
		png_set_tRNS_to_alpha(png);
	if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
		png_set_gray_to_rgb(png);

	png_read_update_info(png, info);

	img->channels = (png_get_color_type(png, info) & PNG_COLOR_MASK_ALPHA) ? 4 : 3;
	if (!image_check_max_pixels(img->width, img->height)) {
		png_destroy_read_struct(&png, &info, NULL);
		fclose(f);
		return false;
	}

	size_t rowbytes = png_get_rowbytes(png, info);
	img->pixels = NULL;
	if (!image_alloc_pixels(img, rowbytes)) {
		png_destroy_read_struct(&png, &info, NULL);
		fclose(f);
		return false;
	}

	size_t rows_bytes;
	if (!checked_mul_size((size_t)img->height, sizeof(png_bytep), &rows_bytes)) {
		free(img->pixels);
		img->pixels = NULL;
		png_destroy_read_struct(&png, &info, NULL);
		fclose(f);
		return false;
	}
	png_bytep *rows = malloc(rows_bytes);
	if (!rows) {
		free(img->pixels);
		img->pixels = NULL;
		png_destroy_read_struct(&png, &info, NULL);
		fclose(f);
		return false;
	}
	for (int y = 0; y < img->height; y++) {
		size_t yoff = (size_t)y * rowbytes;
		rows[y] = img->pixels + yoff;
	}

	png_read_image(png, rows);
	free(rows);

	png_destroy_read_struct(&png, &info, NULL);
	fclose(f);
	return true;
}

static bool png_write(const char *path, struct image *img)
{
	FILE *f = fopen(path, "wb");
	if (!f) return false;

	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!png) {
		fclose(f);
		return false;
	}

	png_infop info = png_create_info_struct(png);
	if (!info) {
		png_destroy_write_struct(&png, NULL);
		fclose(f);
		return false;
	}

	if (setjmp(png_jmpbuf(png))) {
		png_destroy_write_struct(&png, &info);
		fclose(f);
		return false;
	}

	png_init_io(png, f);

	int color_type = (img->channels == 4) ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB;
	png_set_IHDR(png, info, (png_uint_32)img->width, (png_uint_32)img->height, 8, color_type,
				 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_write_info(png, info);

	if (!image_validate_dims(img)) {
		png_destroy_write_struct(&png, &info);
		fclose(f);
		return false;
	}
	size_t rowbytes;
	if (!checked_mul_size((size_t)img->width, (size_t)img->channels, &rowbytes)) {
		png_destroy_write_struct(&png, &info);
		fclose(f);
		return false;
	}
	for (int y = 0; y < img->height; y++) {
		size_t yoff = (size_t)y * rowbytes;
		png_write_row(png, img->pixels + yoff);
	}

	png_write_end(png, NULL);
	png_destroy_write_struct(&png, &info);
	fclose(f);
	return true;
}

// ============================================================================
// JPEG
// ============================================================================

static bool jpeg_read(const char *path, struct image *img)
{
	FILE *f = fopen(path, "rb");
	if (!f) return false;

	struct jpeg_decompress_struct cinfo;
	struct jpeg_error_mgr jerr;

	cinfo.err = jpeg_std_error(&jerr);
	jpeg_create_decompress(&cinfo);
	jpeg_stdio_src(&cinfo, f);
	jpeg_read_header(&cinfo, TRUE);

	cinfo.out_color_space = JCS_RGB;
	jpeg_start_decompress(&cinfo);

	if (cinfo.output_width > (JDIMENSION)INT_MAX || cinfo.output_height > (JDIMENSION)INT_MAX) {
		jpeg_destroy_decompress(&cinfo);
		fclose(f);
		return false;
	}
	img->width = (int)cinfo.output_width;
	img->height = (int)cinfo.output_height;
	img->channels = 3;  // JPEG doesn't support alpha

	if (!image_validate_dims(img)) {
		jpeg_destroy_decompress(&cinfo);
		fclose(f);
		return false;
	}
	if (!image_check_max_pixels(img->width, img->height)) {
		jpeg_destroy_decompress(&cinfo);
		fclose(f);
		return false;
	}
	size_t rowbytes;
	if (!checked_mul_size((size_t)img->width, (size_t)img->channels, &rowbytes)) {
		jpeg_destroy_decompress(&cinfo);
		fclose(f);
		return false;
	}
	img->pixels = NULL;
	if (!image_alloc_pixels(img, rowbytes)) {
		jpeg_destroy_decompress(&cinfo);
		fclose(f);
		return false;
	}

	while (cinfo.output_scanline < cinfo.output_height) {
		uint8_t *row = img->pixels + cinfo.output_scanline * rowbytes;
		jpeg_read_scanlines(&cinfo, &row, 1);
	}

	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	fclose(f);
	return true;
}

static bool jpeg_write(const char *path, struct image *img, int quality)
{
	FILE *f = fopen(path, "wb");
	if (!f) return false;

	// If source has alpha, we need to strip it
	if (!image_validate_dims(img)) {
		fclose(f);
		return false;
	}
	size_t src_rowbytes;
	if (!checked_mul_size((size_t)img->width, (size_t)img->channels, &src_rowbytes)) {
		fclose(f);
		return false;
	}

	struct jpeg_compress_struct cinfo;
	struct jpeg_error_mgr jerr;

	cinfo.err = jpeg_std_error(&jerr);
	jpeg_create_compress(&cinfo);
	jpeg_stdio_dest(&cinfo, f);

	cinfo.image_width = (JDIMENSION)img->width;
	cinfo.image_height = (JDIMENSION)img->height;
	cinfo.input_components = 3;
	cinfo.in_color_space = JCS_RGB;

	jpeg_set_defaults(&cinfo);
	jpeg_set_quality(&cinfo, quality, TRUE);
	jpeg_start_compress(&cinfo, TRUE);

	if (img->channels == 4) {
		size_t rgb_rowbytes;
		if (!checked_mul_size((size_t)img->width, 3, &rgb_rowbytes)) {
			jpeg_destroy_compress(&cinfo);
			fclose(f);
			return false;
		}
		uint8_t *row = malloc(rgb_rowbytes);
		if (!row) {
			jpeg_destroy_compress(&cinfo);
			fclose(f);
			return false;
		}
		for (int y = 0; y < img->height; y++) {
			size_t yoff = (size_t)y * src_rowbytes;
			uint8_t *src = img->pixels + yoff;
			for (int x = 0; x < img->width; x++) {
				row[x * 3 + 0] = src[x * 4 + 0];
				row[x * 3 + 1] = src[x * 4 + 1];
				row[x * 3 + 2] = src[x * 4 + 2];
			}
			jpeg_write_scanlines(&cinfo, &row, 1);
		}
		free(row);
	} else {
		for (int y = 0; y < img->height; y++) {
			size_t yoff = (size_t)y * src_rowbytes;
			uint8_t *row = img->pixels + yoff;
			jpeg_write_scanlines(&cinfo, &row, 1);
		}
	}

	jpeg_finish_compress(&cinfo);
	jpeg_destroy_compress(&cinfo);
	fclose(f);
	return true;
}

// ============================================================================
// WebP
// ============================================================================

#ifdef HAVE_WEBP
static bool webp_read(const char *path, struct image *img)
{
	size_t size;
	uint8_t *data = read_file(path, &size);
	if (!data) return false;

	// Check if image has alpha
	WebPBitstreamFeatures features;
	if (WebPGetFeatures(data, size, &features) != VP8_STATUS_OK) {
		free(data);
		return false;
	}

	if (features.width <= 0 || features.height <= 0) {
		free(data);
		return false;
	}
	if (!image_check_max_pixels(features.width, features.height)) {
		free(data);
		return false;
	}
	img->width = features.width;
	img->height = features.height;

	if (features.has_alpha) {
		img->channels = 4;
		img->pixels = WebPDecodeRGBA(data, size, &img->width, &img->height);
	} else {
		img->channels = 3;
		img->pixels = WebPDecodeRGB(data, size, &img->width, &img->height);
	}

	free(data);

	if (!img->pixels)
		return false;

	// WebP allocates with its own allocator, copy to our malloc'd buffer
	size_t pixel_count;
	if (!checked_mul_size((size_t)img->width, (size_t)img->height, &pixel_count)) {
		WebPFree(img->pixels);
		return false;
	}
	size_t pixel_size;
	if (!checked_mul_size(pixel_count, (size_t)img->channels, &pixel_size)) {
		WebPFree(img->pixels);
		return false;
	}
	uint8_t *copy = malloc(pixel_size);
	if (!copy) {
		WebPFree(img->pixels);
		return false;
	}
	memcpy(copy, img->pixels, pixel_size);
	WebPFree(img->pixels);
	img->pixels = copy;

	return true;
}

static bool webp_write(const char *path, struct image *img, int quality)
{
	uint8_t *output = NULL;
	size_t output_size;

	if (!image_validate_dims(img))
		return false;
	if (img->width > INT_MAX / img->channels)
		return false;
	int stride = img->width * img->channels;

	if (img->channels == 4) {
		output_size = WebPEncodeRGBA(img->pixels, img->width, img->height, stride, quality, &output);
	} else {
		output_size = WebPEncodeRGB(img->pixels, img->width, img->height, stride, quality, &output);
	}

	if (output_size == 0)
		return false;

	FILE *f = fopen(path, "wb");
	if (!f) {
		WebPFree(output);
		return false;
	}

	bool ok = WRITE_FILE(output, output_size, f) == output_size;
	fclose(f);
	WebPFree(output);

	return ok;
}
#endif

// ============================================================================
// BMP (no library needed)
// ============================================================================

#pragma pack(push, 1)
struct bmp_file_header {
	uint16_t type;
	uint32_t size;
	uint16_t reserved1;
	uint16_t reserved2;
	uint32_t offset;
};

struct bmp_info_header {
	uint32_t size;
	int32_t width;
	int32_t height;
	uint16_t planes;
	uint16_t bit_count;
	uint32_t compression;
	uint32_t image_size;
	int32_t x_pels_per_meter;
	int32_t y_pels_per_meter;
	uint32_t clr_used;
	uint32_t clr_important;
};
#pragma pack(pop)

static bool bmp_read(const char *path, struct image *img)
{
	FILE *f = fopen(path, "rb");
	if (!f) return false;

	struct bmp_file_header fh;
	struct bmp_info_header ih;

	if (READ_FILE(&fh, sizeof(fh), f) != sizeof(fh) || READ_FILE(&ih, sizeof(ih), f) != sizeof(ih)) {
		fclose(f);
		return false;
	}

	if (fh.type != 0x4D42) {  // "BM"
		fclose(f);
		return false;
	}

	// Only support uncompressed 24-bit or 32-bit BMPs
	if (ih.compression != 0 || (ih.bit_count != 24 && ih.bit_count != 32)) {
		fclose(f);
		return false;
	}

	if (ih.width <= 0 || ih.width > INT_MAX || ih.height == 0 || ih.height == INT32_MIN) {
		fclose(f);
		return false;
	}

	bool top_down = ih.height < 0;
	img->width = ih.width;
	img->height = top_down ? -ih.height : ih.height;
	img->channels = (ih.bit_count == 32) ? 4 : 3;

	if (!image_validate_dims(img)) {
		fclose(f);
		return false;
	}
	if (!image_check_max_pixels(img->width, img->height)) {
		fclose(f);
		return false;
	}
	size_t rowbytes;
	if (!checked_mul_size((size_t)img->width, (size_t)img->channels, &rowbytes)) {
		fclose(f);
		return false;
	}
	img->pixels = NULL;
	if (!image_alloc_pixels(img, rowbytes)) {
		fclose(f);
		return false;
	}

	// BMP rows are padded to 4-byte boundaries
	int bmp_channels = ih.bit_count / 8;
	size_t raw_rowbytes;
	if (!checked_mul_size((size_t)img->width, (size_t)bmp_channels, &raw_rowbytes)) {
		free(img->pixels);
		img->pixels = NULL;
		fclose(f);
		return false;
	}
	size_t tmp_rowbytes;
	if (!checked_add_size(raw_rowbytes, 3, &tmp_rowbytes)) {
		free(img->pixels);
		img->pixels = NULL;
		fclose(f);
		return false;
	}
	size_t bmp_rowbytes = tmp_rowbytes & ~(size_t)3;
	uint8_t *row = malloc(bmp_rowbytes);
	if (!row) {
		free(img->pixels);
		img->pixels = NULL;
		fclose(f);
		return false;
	}

	if (fseek(f, fh.offset, SEEK_SET) != 0) {
		free(row);
		free(img->pixels);
		img->pixels = NULL;
		fclose(f);
		return false;
	}

	for (int y = 0; y < img->height; y++) {
		size_t dst_y = (size_t)(top_down ? y : (img->height - 1 - y));
		if (READ_FILE(row, bmp_rowbytes, f) != bmp_rowbytes) {
			free(row);
			free(img->pixels);
			fclose(f);
			return false;
		}

		uint8_t *dst = img->pixels + dst_y * rowbytes;
		for (int x = 0; x < img->width; x++) {
			// BMP is BGR(A), convert to RGB(A)
			dst[x * img->channels + 0] = row[x * bmp_channels + 2];
			dst[x * img->channels + 1] = row[x * bmp_channels + 1];
			dst[x * img->channels + 2] = row[x * bmp_channels + 0];
			if (img->channels == 4)
				dst[x * img->channels + 3] = row[x * bmp_channels + 3];
		}
	}

	free(row);
	fclose(f);
	return true;
}

static bool bmp_write(const char *path, struct image *img)
{
	FILE *f = fopen(path, "wb");
	if (!f) return false;

	if (!image_validate_dims(img)) {
		fclose(f);
		return false;
	}

	int bmp_channels = 3;  // Always write 24-bit BMP (no alpha)
	size_t raw_rowbytes;
	if (!checked_mul_size((size_t)img->width, (size_t)bmp_channels, &raw_rowbytes)) {
		fclose(f);
		return false;
	}
	size_t tmp_rowbytes;
	if (!checked_add_size(raw_rowbytes, 3, &tmp_rowbytes)) {
		fclose(f);
		return false;
	}
	size_t bmp_rowbytes = tmp_rowbytes & ~(size_t)3;
	if (bmp_rowbytes == 0) {
		fclose(f);
		return false;
	}
	size_t image_size;
	if (!checked_mul_size(bmp_rowbytes, (size_t)img->height, &image_size)) {
		fclose(f);
		return false;
	}
	if (image_size > UINT32_MAX - (sizeof(struct bmp_file_header) + sizeof(struct bmp_info_header))) {
		fclose(f);
		return false;
	}

	struct bmp_file_header fh = {
		.type = 0x4D42,
		.size = (uint32_t)(sizeof(fh) + sizeof(struct bmp_info_header) + image_size),
		.offset = sizeof(fh) + sizeof(struct bmp_info_header)
	};

	struct bmp_info_header ih = {
		.size = sizeof(ih),
		.width = img->width,
		.height = img->height,
		.planes = 1,
		.bit_count = 24,
		.image_size = (uint32_t)image_size
	};

	if (WRITE_FILE(&fh, sizeof(fh), f) != sizeof(fh) || WRITE_FILE(&ih, sizeof(ih), f) != sizeof(ih)) {
		fclose(f);
		return false;
	}

	uint8_t *row = calloc(1, bmp_rowbytes);
	if (!row) {
		fclose(f);
		return false;
	}

	size_t src_rowbytes;
	if (!checked_mul_size((size_t)img->width, (size_t)img->channels, &src_rowbytes)) {
		free(row);
		fclose(f);
		return false;
	}

	for (int y = img->height - 1; y >= 0; y--) {
		size_t yoff = (size_t)y * src_rowbytes;
		uint8_t *src = img->pixels + yoff;
		for (int x = 0; x < img->width; x++) {
			// RGB(A) to BGR
			row[x * 3 + 0] = src[x * img->channels + 2];
			row[x * 3 + 1] = src[x * img->channels + 1];
			row[x * 3 + 2] = src[x * img->channels + 0];
		}
		if (WRITE_FILE(row, bmp_rowbytes, f) != bmp_rowbytes) {
			free(row);
			fclose(f);
			return false;
		}
	}

	free(row);
	fclose(f);
	return true;
}

// ============================================================================
// QOI (no library needed)
// ============================================================================

static bool qoi_read(const char *path, struct image *img)
{
	FILE *f = fopen(path, "rb");
	if (!f) return false;

	uint8_t header[QOI_HEADER_SIZE];
	if (!read_bytes(f, header, sizeof(header))) {
		fclose(f);
		return false;
	}

	if (qoi_read32be(header) != QOI_MAGIC) {
		fclose(f);
		return false;
	}

	uint32_t width = qoi_read32be(header + 4);
	uint32_t height = qoi_read32be(header + 8);
	img->channels = header[12];

	if (width == 0 || height == 0 || width > INT_MAX || height > INT_MAX) {
		fclose(f);
		return false;
	}

	if (img->channels != 3 && img->channels != 4) {
		fclose(f);
		return false;
	}

	size_t pixel_count;
	if (!checked_mul_size((size_t)width, (size_t)height, &pixel_count)) {
		fclose(f);
		return false;
	}
	size_t pixel_bytes;
	if (!checked_mul_size(pixel_count, (size_t)img->channels, &pixel_bytes)) {
		fclose(f);
		return false;
	}

	img->width = (int)width;
	img->height = (int)height;
	if (!image_check_max_pixels(img->width, img->height)) {
		fclose(f);
		return false;
	}

	img->pixels = malloc(pixel_bytes);
	if (!img->pixels) {
		fclose(f);
		return false;
	}

	uint8_t index[64][4] = {0};
	uint8_t px[4] = {0, 0, 0, 255};
	size_t px_pos = 0;
	size_t px_end = pixel_count * (size_t)img->channels;

	while (px_pos < px_end) {
		uint8_t b1;
		if (!read_byte(f, &b1)) {
			free(img->pixels);
			fclose(f);
			return false;
		}

		if (b1 == QOI_OP_RGB) {
			if (!read_bytes(f, px, 3)) {
				free(img->pixels);
				fclose(f);
				return false;
			}
		} else if (b1 == QOI_OP_RGBA) {
			if (!read_bytes(f, px, 4)) {
				free(img->pixels);
				fclose(f);
				return false;
			}
		} else if ((b1 & QOI_MASK_2) == QOI_OP_INDEX) {
			memcpy(px, index[b1], 4);
		} else if ((b1 & QOI_MASK_2) == QOI_OP_DIFF) {
			px[0] += ((b1 >> 4) & 0x03) - 2;
			px[1] += ((b1 >> 2) & 0x03) - 2;
			px[2] += (b1 & 0x03) - 2;
		} else if ((b1 & QOI_MASK_2) == QOI_OP_LUMA) {
			uint8_t b2;
			if (!read_byte(f, &b2)) {
				free(img->pixels);
				fclose(f);
				return false;
			}
			int vg = (b1 & 0x3f) - 32;
			px[0] += vg - 8 + ((b2 >> 4) & 0x0f);
			px[1] += vg;
			px[2] += vg - 8 + (b2 & 0x0f);
		} else if ((b1 & QOI_MASK_2) == QOI_OP_RUN) {
			int run = (b1 & 0x3f) + 1;
			while (run-- > 0 && px_pos < px_end) {
				memcpy(img->pixels + px_pos, px, (size_t)img->channels);
				px_pos += (size_t)img->channels;
			}
			continue;
		}

		memcpy(index[qoi_hash(px[0], px[1], px[2], px[3])], px, 4);
		memcpy(img->pixels + px_pos, px, (size_t)img->channels);
		px_pos += (size_t)img->channels;
	}

	fclose(f);
	return true;
}

static bool qoi_write(const char *path, struct image *img)
{
	if (!image_validate_dims(img))
		return false;

	size_t pixel_count;
	if (!checked_mul_size((size_t)img->width, (size_t)img->height, &pixel_count))
		return false;

	size_t per_pixel = (size_t)img->channels + 1;
	size_t data_bytes;
	if (!checked_mul_size(pixel_count, per_pixel, &data_bytes))
		return false;

	size_t max_size;
	if (!checked_add_size(QOI_HEADER_SIZE, data_bytes, &max_size))
		return false;
	if (!checked_add_size(max_size, QOI_END_MARKER_SIZE, &max_size))
		return false;

	uint8_t *data = malloc(max_size);
	if (!data) return false;

	qoi_write32be(data, QOI_MAGIC);
	qoi_write32be(data + 4, (uint32_t)img->width);
	qoi_write32be(data + 8, (uint32_t)img->height);
	data[12] = (uint8_t)img->channels;
	data[13] = 1;  // colorspace: sRGB

	uint8_t index[64][4] = {0};
	uint8_t px[4] = {0, 0, 0, 255};
	uint8_t px_prev[4] = {0, 0, 0, 255};
	size_t p = QOI_HEADER_SIZE;
	int run = 0;

	for (size_t i = 0; i < pixel_count; i++) {
		memcpy(px, img->pixels + i * (size_t)img->channels, (size_t)img->channels);
		if (img->channels == 3) px[3] = 255;

		if (memcmp(px, px_prev, 4) == 0) {
			run++;
				if (run == 62 || i == pixel_count - 1) {
					data[p++] = (uint8_t)(QOI_OP_RUN | (run - 1));
					run = 0;
				}
			} else {
				if (run > 0) {
					data[p++] = (uint8_t)(QOI_OP_RUN | (run - 1));
					run = 0;
				}

			int idx = qoi_hash(px[0], px[1], px[2], px[3]);
				if (memcmp(index[idx], px, 4) == 0) {
					data[p++] = (uint8_t)(QOI_OP_INDEX | idx);
				} else {
				memcpy(index[idx], px, 4);

				if (px[3] == px_prev[3]) {
					int vr = px[0] - px_prev[0];
					int vg = px[1] - px_prev[1];
					int vb = px[2] - px_prev[2];

					int vg_r = vr - vg;
					int vg_b = vb - vg;

						if (vr > -3 && vr < 2 && vg > -3 && vg < 2 && vb > -3 && vb < 2) {
							data[p++] = (uint8_t)(QOI_OP_DIFF | ((vr + 2) << 4) | ((vg + 2) << 2) | (vb + 2));
						} else if (vg_r > -9 && vg_r < 8 && vg > -33 && vg < 32 && vg_b > -9 && vg_b < 8) {
							data[p++] = (uint8_t)(QOI_OP_LUMA | (vg + 32));
							data[p++] = (uint8_t)(((vg_r + 8) << 4) | (vg_b + 8));
					} else {
						data[p++] = QOI_OP_RGB;
						data[p++] = px[0];
						data[p++] = px[1];
						data[p++] = px[2];
					}
				} else {
					data[p++] = QOI_OP_RGBA;
					data[p++] = px[0];
					data[p++] = px[1];
					data[p++] = px[2];
					data[p++] = px[3];
				}
			}
		}
		memcpy(px_prev, px, 4);
	}

	// End marker
	memset(data + p, 0, 7);
	data[p + 7] = 1;
	p += 8;

	FILE *f = fopen(path, "wb");
	if (!f) {
		free(data);
		return false;
	}

	bool ok = WRITE_FILE(data, p, f) == p;
	fclose(f);
	free(data);
	return ok;
}

// ============================================================================
// AVIF (libavif)
// ============================================================================

#ifdef HAVE_AVIF
static bool avif_read(const char *path, struct image *img)
{
	size_t size;
	uint8_t *data = read_file(path, &size);
	if (!data) return false;

	avifDecoder *decoder = avifDecoderCreate();
	if (!decoder) {
		free(data);
		return false;
	}

	avifResult result = avifDecoderSetIOMemory(decoder, data, size);
	if (result != AVIF_RESULT_OK) {
		avifDecoderDestroy(decoder);
		free(data);
		return false;
	}

	result = avifDecoderParse(decoder);
	if (result != AVIF_RESULT_OK) {
		avifDecoderDestroy(decoder);
		free(data);
		return false;
	}

	result = avifDecoderNextImage(decoder);
	if (result != AVIF_RESULT_OK) {
		avifDecoderDestroy(decoder);
		free(data);
		return false;
	}

	avifImage *avif = decoder->image;
	if (avif->width == 0 || avif->height == 0 || avif->width > INT_MAX || avif->height > INT_MAX) {
		avifDecoderDestroy(decoder);
		free(data);
		return false;
	}
	img->width = (int)avif->width;
	img->height = (int)avif->height;
	img->channels = avif->alphaPlane ? 4 : 3;
	if (!image_check_max_pixels(img->width, img->height)) {
		avifDecoderDestroy(decoder);
		free(data);
		return false;
	}

	size_t rowbytes;
	if (!checked_mul_size((size_t)img->width, (size_t)img->channels, &rowbytes)) {
		avifDecoderDestroy(decoder);
		free(data);
		return false;
	}
	img->pixels = NULL;
	if (!image_alloc_pixels(img, rowbytes)) {
		avifDecoderDestroy(decoder);
		free(data);
		return false;
	}

	avifRGBImage rgb;
	avifRGBImageSetDefaults(&rgb, avif);
	rgb.format = img->channels == 4 ? AVIF_RGB_FORMAT_RGBA : AVIF_RGB_FORMAT_RGB;
	rgb.depth = 8;
	rgb.pixels = img->pixels;
	rgb.rowBytes = rowbytes;

	result = avifImageYUVToRGB(avif, &rgb);
	avifDecoderDestroy(decoder);
	free(data);

	if (result != AVIF_RESULT_OK) {
		free(img->pixels);
		img->pixels = NULL;
		return false;
	}

	return true;
}

static bool avif_write(const char *path, struct image *img, int quality)
{
	if (!image_validate_dims(img))
		return false;
	if (img->width > INT_MAX || img->height > INT_MAX)
		return false;

	avifImage *avif = avifImageCreate(img->width, img->height, 8, AVIF_PIXEL_FORMAT_YUV444);
	if (!avif) return false;

	avifRGBImage rgb;
	avifRGBImageSetDefaults(&rgb, avif);
	rgb.format = img->channels == 4 ? AVIF_RGB_FORMAT_RGBA : AVIF_RGB_FORMAT_RGB;
	rgb.depth = 8;
	rgb.pixels = img->pixels;
	size_t rowbytes;
	if (!checked_mul_size((size_t)img->width, (size_t)img->channels, &rowbytes)) {
		avifImageDestroy(avif);
		return false;
	}
	rgb.rowBytes = rowbytes;

	avifResult result = avifImageRGBToYUV(avif, &rgb);
	if (result != AVIF_RESULT_OK) {
		avifImageDestroy(avif);
		return false;
	}

	avifEncoder *encoder = avifEncoderCreate();
	if (!encoder) {
		avifImageDestroy(avif);
		return false;
	}

	encoder->quality = quality;
	encoder->speed = AVIF_SPEED_DEFAULT;

	avifRWData output = AVIF_DATA_EMPTY;
	result = avifEncoderWrite(encoder, avif, &output);
	avifEncoderDestroy(encoder);
	avifImageDestroy(avif);

	if (result != AVIF_RESULT_OK) {
		avifRWDataFree(&output);
		return false;
	}

	FILE *f = fopen(path, "wb");
	if (!f) {
		avifRWDataFree(&output);
		return false;
	}

	bool ok = WRITE_FILE(output.data, output.size, f) == output.size;
	fclose(f);
	avifRWDataFree(&output);

	return ok;
}
#endif

// ============================================================================
// HEIF (libheif)
// ============================================================================

#ifdef HAVE_HEIF
static bool heif_read(const char *path, struct image *img)
{
	heif_context *ctx = heif_context_alloc();
	if (!ctx) return false;

	struct heif_error err = heif_context_read_from_file(ctx, path, NULL);
	if (err.code != heif_error_Ok) {
		heif_context_free(ctx);
		return false;
	}

	heif_image_handle *handle;
	err = heif_context_get_primary_image_handle(ctx, &handle);
	if (err.code != heif_error_Ok) {
		heif_context_free(ctx);
		return false;
	}

	bool has_alpha = heif_image_handle_has_alpha_channel(handle);
	img->channels = has_alpha ? 4 : 3;

	heif_image *heif_img;
	err = heif_decode_image(handle, &heif_img,
							heif_colorspace_RGB,
							has_alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB,
							NULL);
	heif_image_handle_release(handle);

	if (err.code != heif_error_Ok) {
		heif_context_free(ctx);
		return false;
	}

	img->width = heif_image_get_width(heif_img, heif_channel_interleaved);
	img->height = heif_image_get_height(heif_img, heif_channel_interleaved);

	int stride;
	const uint8_t *data = heif_image_get_plane_readonly(heif_img, heif_channel_interleaved, &stride);

	if (!data || stride <= 0 || !image_validate_dims(img) || img->width > INT_MAX || img->height > INT_MAX) {
		heif_image_release(heif_img);
		heif_context_free(ctx);
		return false;
	}
	if (!image_check_max_pixels(img->width, img->height)) {
		heif_image_release(heif_img);
		heif_context_free(ctx);
		return false;
	}

	size_t rowbytes;
	if (!checked_mul_size((size_t)img->width, (size_t)img->channels, &rowbytes)) {
		heif_image_release(heif_img);
		heif_context_free(ctx);
		return false;
	}
	img->pixels = NULL;
	if (!image_alloc_pixels(img, rowbytes)) {
		heif_image_release(heif_img);
		heif_context_free(ctx);
		return false;
	}

	for (int y = 0; y < img->height; y++) {
		memcpy(img->pixels + y * rowbytes, data + y * stride, rowbytes);
	}

	heif_image_release(heif_img);
	heif_context_free(ctx);
	return true;
}

static bool heif_write(const char *path, struct image *img, int quality)
{
	heif_context *ctx = heif_context_alloc();
	if (!ctx) return false;

	if (!image_validate_dims(img)) {
		heif_context_free(ctx);
		return false;
	}

	heif_encoder *encoder;
	struct heif_error err = heif_context_get_encoder_for_format(ctx, heif_compression_HEVC, &encoder);
	if (err.code != heif_error_Ok) {
		heif_context_free(ctx);
		return false;
	}

	heif_encoder_set_lossy_quality(encoder, quality);

	heif_image *heif_img;
	err = heif_image_create(img->width, img->height, heif_colorspace_RGB,
							img->channels == 4 ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB,
							&heif_img);
	if (err.code != heif_error_Ok) {
		heif_encoder_release(encoder);
		heif_context_free(ctx);
		return false;
	}

	err = heif_image_add_plane(heif_img, heif_channel_interleaved, img->width, img->height, 8);
	if (err.code != heif_error_Ok) {
		heif_image_release(heif_img);
		heif_encoder_release(encoder);
		heif_context_free(ctx);
		return false;
	}

	int stride;
	uint8_t *data = heif_image_get_plane(heif_img, heif_channel_interleaved, &stride);
	if (!data || stride <= 0) {
		heif_image_release(heif_img);
		heif_encoder_release(encoder);
		heif_context_free(ctx);
		return false;
	}

	size_t rowbytes;
	if (!checked_mul_size((size_t)img->width, (size_t)img->channels, &rowbytes)) {
		heif_image_release(heif_img);
		heif_encoder_release(encoder);
		heif_context_free(ctx);
		return false;
	}

	for (int y = 0; y < img->height; y++) {
		memcpy(data + y * stride, img->pixels + y * rowbytes, rowbytes);
	}

	err = heif_context_encode_image(ctx, heif_img, encoder, NULL, NULL);
	heif_image_release(heif_img);
	heif_encoder_release(encoder);

	if (err.code != heif_error_Ok) {
		heif_context_free(ctx);
		return false;
	}

	err = heif_context_write_to_file(ctx, path);
	heif_context_free(ctx);

	return err.code == heif_error_Ok;
}
#endif

// ============================================================================
// TIFF (libtiff)
// ============================================================================

#ifdef HAVE_TIFF
static bool tiff_read(const char *path, struct image *img)
{
	TIFF *tif = TIFFOpen(path, "r");
	if (!tif) return false;

	uint32_t w, h;
	TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
	TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);

	if (w == 0 || h == 0 || w > INT_MAX || h > INT_MAX) {
		TIFFClose(tif);
		return false;
	}
	img->width = (int)w;
	img->height = (int)h;
	img->channels = 4;  // TIFFReadRGBAImage always outputs RGBA
	if (!image_check_max_pixels(img->width, img->height)) {
		TIFFClose(tif);
		return false;
	}

	size_t pixel_count;
	if (!checked_mul_size((size_t)w, (size_t)h, &pixel_count) ||
		pixel_count > SIZE_MAX / sizeof(uint32_t)) {
		TIFFClose(tif);
		return false;
	}
	uint32_t *raster = malloc(pixel_count * sizeof(uint32_t));
	if (!raster) {
		TIFFClose(tif);
		return false;
	}

	if (!TIFFReadRGBAImageOriented(tif, w, h, raster, ORIENTATION_TOPLEFT, 0)) {
		free(raster);
		TIFFClose(tif);
		return false;
	}
	TIFFClose(tif);

	// Convert from ABGR (libtiff native) to RGBA
	size_t pixel_bytes;
	if (!checked_mul_size(pixel_count, 4, &pixel_bytes)) {
		free(raster);
		return false;
	}
	img->pixels = malloc(pixel_bytes);
	if (!img->pixels) {
		free(raster);
		return false;
	}

	for (size_t i = 0; i < pixel_count; i++) {
		uint32_t p = raster[i];
		img->pixels[i * 4 + 0] = TIFFGetR(p);
		img->pixels[i * 4 + 1] = TIFFGetG(p);
		img->pixels[i * 4 + 2] = TIFFGetB(p);
		img->pixels[i * 4 + 3] = TIFFGetA(p);
	}

	free(raster);
	return true;
}

static bool tiff_write(const char *path, struct image *img)
{
	TIFF *tif = TIFFOpen(path, "w");
	if (!tif) return false;

	if (!image_validate_dims(img)) {
		TIFFClose(tif);
		return false;
	}

	TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, img->width);
	TIFFSetField(tif, TIFFTAG_IMAGELENGTH, img->height);
	TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, img->channels);
	TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
	TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
	TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
	TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
	TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_LZW);

	if (img->channels == 4) {
		uint16_t extra = EXTRASAMPLE_ASSOCALPHA;
		TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, 1, &extra);
	}

	size_t rowbytes;
	if (!checked_mul_size((size_t)img->width, (size_t)img->channels, &rowbytes)) {
		TIFFClose(tif);
		return false;
	}
	for (int y = 0; y < img->height; y++) {
		if (TIFFWriteScanline(tif, img->pixels + y * rowbytes, y, 0) < 0) {
			TIFFClose(tif);
			return false;
		}
	}

	TIFFClose(tif);
	return true;
}
#endif

// ============================================================================
// JPEG XL (libjxl)
// ============================================================================

#ifdef HAVE_JXL
static bool jxl_read(const char *path, struct image *img)
{
	size_t size;
	uint8_t *data = read_file(path, &size);
	if (!data) return false;

	JxlDecoder *dec = JxlDecoderCreate(NULL);
	if (!dec) {
		free(data);
		return false;
	}

	void *runner = JxlResizableParallelRunnerCreate(NULL);
	if (JxlDecoderSetParallelRunner(dec, JxlResizableParallelRunner, runner) != JXL_DEC_SUCCESS) {
		JxlResizableParallelRunnerDestroy(runner);
		JxlDecoderDestroy(dec);
		free(data);
		return false;
	}

	if (JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) {
		JxlResizableParallelRunnerDestroy(runner);
		JxlDecoderDestroy(dec);
		free(data);
		return false;
	}

	JxlDecoderSetInput(dec, data, size);
	JxlDecoderCloseInput(dec);

	JxlBasicInfo info;
	JxlPixelFormat format = {0, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
	bool success = false;

	for (;;) {
		JxlDecoderStatus status = JxlDecoderProcessInput(dec);

			if (status == JXL_DEC_BASIC_INFO) {
				if (JxlDecoderGetBasicInfo(dec, &info) != JXL_DEC_SUCCESS)
					break;

				if (info.xsize == 0 || info.ysize == 0 || info.xsize > INT_MAX || info.ysize > INT_MAX)
					break;
				img->width = (int)info.xsize;
				img->height = (int)info.ysize;
				img->channels = info.alpha_bits > 0 ? 4 : 3;
				if (!image_check_max_pixels(img->width, img->height))
					break;
				format.num_channels = img->channels;

				JxlResizableParallelRunnerSetThreads(runner,
					JxlResizableParallelRunnerSuggestThreads(info.xsize, info.ysize));

			} else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
				size_t buffer_size;
				if (JxlDecoderImageOutBufferSize(dec, &format, &buffer_size) != JXL_DEC_SUCCESS)
					break;

				if (!image_validate_dims(img))
					break;
				size_t pixel_count;
				if (!checked_mul_size((size_t)img->width, (size_t)img->height, &pixel_count))
					break;
				size_t expected;
				if (!checked_mul_size(pixel_count, (size_t)img->channels, &expected))
					break;
				if (buffer_size < expected)
					break;

				img->pixels = malloc(buffer_size);
				if (!img->pixels)
					break;

				if (JxlDecoderSetImageOutBuffer(dec, &format, img->pixels, buffer_size) != JXL_DEC_SUCCESS) {
					free(img->pixels);
					img->pixels = NULL;
					break;
				}

			} else if (status == JXL_DEC_FULL_IMAGE) {
				success = true;
			break;

		} else if (status == JXL_DEC_SUCCESS) {
			success = true;
			break;

		} else {
			break;
		}
	}

		JxlResizableParallelRunnerDestroy(runner);
		JxlDecoderDestroy(dec);
		free(data);

		if (!success && img->pixels) {
			free(img->pixels);
			img->pixels = NULL;
		}

		return success;
	}

	static bool jxl_write(const char *path, struct image *img, int quality)
	{
		if (!image_validate_dims(img))
			return false;

		JxlEncoder *enc = JxlEncoderCreate(NULL);
		if (!enc) return false;

		void *runner = JxlResizableParallelRunnerCreate(NULL);
		if (JxlEncoderSetParallelRunner(enc, JxlResizableParallelRunner, runner) != JXL_ENC_SUCCESS) {
			JxlResizableParallelRunnerDestroy(runner);
			JxlEncoderDestroy(enc);
			return false;
		}

		JxlBasicInfo info;
		JxlEncoderInitBasicInfo(&info);
		info.xsize = img->width;
		info.ysize = img->height;
		info.bits_per_sample = 8;
		info.num_color_channels = 3;
		info.num_extra_channels = img->channels == 4 ? 1 : 0;
		info.alpha_bits = img->channels == 4 ? 8 : 0;
		info.uses_original_profile = JXL_FALSE;

		if (JxlEncoderSetBasicInfo(enc, &info) != JXL_ENC_SUCCESS) {
			JxlResizableParallelRunnerDestroy(runner);
			JxlEncoderDestroy(enc);
			return false;
		}

		JxlColorEncoding color;
		JxlColorEncodingSetToSRGB(&color, JXL_FALSE);
		if (JxlEncoderSetColorEncoding(enc, &color) != JXL_ENC_SUCCESS) {
			JxlResizableParallelRunnerDestroy(runner);
			JxlEncoderDestroy(enc);
			return false;
		}

		JxlEncoderFrameSettings *settings = JxlEncoderFrameSettingsCreate(enc, NULL);

		// Convert quality 1-100 to distance (0 = lossless, 1 = visually lossless, 15 = max lossy)
		float distance = quality >= 100 ? 0.0f : (100.0f - quality) / 6.5f;
		if (distance > 15.0f) distance = 15.0f;
		JxlEncoderSetFrameDistance(settings, distance);

		JxlPixelFormat format = {
			.num_channels = img->channels,
			.data_type = JXL_TYPE_UINT8,
			.endianness = JXL_NATIVE_ENDIAN,
			.align = 0
		};

		size_t pixel_count;
		if (!checked_mul_size((size_t)img->width, (size_t)img->height, &pixel_count)) {
			JxlResizableParallelRunnerDestroy(runner);
			JxlEncoderDestroy(enc);
			return false;
		}
		size_t pixel_size;
		if (!checked_mul_size(pixel_count, (size_t)img->channels, &pixel_size)) {
			JxlResizableParallelRunnerDestroy(runner);
			JxlEncoderDestroy(enc);
			return false;
		}
		if (JxlEncoderAddImageFrame(settings, &format, img->pixels, pixel_size) != JXL_ENC_SUCCESS) {
			JxlResizableParallelRunnerDestroy(runner);
			JxlEncoderDestroy(enc);
			return false;
		}

		JxlEncoderCloseInput(enc);

		// Encode to buffer
		size_t output_cap = 4096;
		size_t output_size = 0;
		uint8_t *output = malloc(output_cap);
		if (!output) {
			JxlResizableParallelRunnerDestroy(runner);
			JxlEncoderDestroy(enc);
			return false;
		}

		for (;;) {
			uint8_t *next_out = output + output_size;
			size_t avail_out = output_cap - output_size;

			JxlEncoderStatus status = JxlEncoderProcessOutput(enc, &next_out, &avail_out);
			output_size = next_out - output;

			if (status == JXL_ENC_SUCCESS) {
				break;
			} else if (status == JXL_ENC_NEED_MORE_OUTPUT) {
				if (output_cap > SIZE_MAX / 2) {
					free(output);
					JxlResizableParallelRunnerDestroy(runner);
					JxlEncoderDestroy(enc);
					return false;
				}
				output_cap *= 2;
				uint8_t *new_output = realloc(output, output_cap);
				if (!new_output) {
					free(output);
					JxlResizableParallelRunnerDestroy(runner);
					JxlEncoderDestroy(enc);
					return false;
				}
				output = new_output;
			} else {
				free(output);
				JxlResizableParallelRunnerDestroy(runner);
				JxlEncoderDestroy(enc);
				return false;
			}
		}

		JxlResizableParallelRunnerDestroy(runner);
		JxlEncoderDestroy(enc);

		FILE *f = fopen(path, "wb");
		if (!f) {
			free(output);
			return false;
		}

		bool ok = WRITE_FILE(output, output_size, f) == output_size;
		fclose(f);
		free(output);

		return ok;
	}
#endif

// ============================================================================
// Format detection
// ============================================================================

enum format {
	FMT_UNKNOWN,
	FMT_PNG,
	FMT_JPEG,
	FMT_BMP,
	FMT_QOI,
#ifdef HAVE_TIFF
	FMT_TIFF,
#endif
#ifdef HAVE_WEBP
	FMT_WEBP,
#endif
#ifdef HAVE_AVIF
	FMT_AVIF,
#endif
#ifdef HAVE_HEIF
	FMT_HEIF,
#endif
#ifdef HAVE_JXL
	FMT_JXL,
#endif
};

static enum format format_from_name(const char *name)
{
	if (strcasecmp(name, "png") == 0)
		return FMT_PNG;
	if (strcasecmp(name, "jpg") == 0 || strcasecmp(name, "jpeg") == 0)
		return FMT_JPEG;
	if (strcasecmp(name, "bmp") == 0)
		return FMT_BMP;
	if (strcasecmp(name, "qoi") == 0)
		return FMT_QOI;
#ifdef HAVE_TIFF
	if (strcasecmp(name, "tiff") == 0 || strcasecmp(name, "tif") == 0)
		return FMT_TIFF;
#endif
#ifdef HAVE_WEBP
	if (strcasecmp(name, "webp") == 0)
		return FMT_WEBP;
#endif
#ifdef HAVE_AVIF
	if (strcasecmp(name, "avif") == 0)
		return FMT_AVIF;
#endif
#ifdef HAVE_HEIF
	if (strcasecmp(name, "heif") == 0 || strcasecmp(name, "heic") == 0)
		return FMT_HEIF;
#endif
#ifdef HAVE_JXL
	if (strcasecmp(name, "jxl") == 0)
		return FMT_JXL;
#endif
	return FMT_UNKNOWN;
}

static enum format detect_format(const char *path)
{
	const char *ext = strrchr(path, '.');
	if (!ext) return FMT_UNKNOWN;
	return format_from_name(ext + 1);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv)
{
			struct option options[] = {
				{ "format", required_argument, 0, 'f' },
				{ "quality", required_argument, 0, 'q' },
				{ "output", required_argument, 0, 'o' },
				{ "max-pixels", required_argument, 0, 'm' },
				{ "max-bytes", required_argument, 0, 'B' },
				{ "help", no_argument, 0, 'h' },
				{ 0 }
			};

	enum format to_fmt = FMT_UNKNOWN;
	const char *output_path = NULL;
	int quality = 85;

	int c;
	while ((c = getopt_long(argc, argv, "f:q:o:m:B:h", options, NULL)) != -1) {
		switch (c) {
		case 'f':
			to_fmt = format_from_name(optarg);
			if (to_fmt == FMT_UNKNOWN) {
				PRINTF_ERR("Unknown format: %s\n", optarg);
				return EXIT_FAILURE;
			}
			break;
		case 'q': {
			char *end;
			errno = 0;
			long val = strtol(optarg, &end, 10);
			if (errno != 0 || end == optarg || *end != '\0') {
				PRINTF_ERR("Invalid quality: %s\n", optarg);
				return EXIT_FAILURE;
			}
			if (val < 1) val = 1;
			if (val > 100) val = 100;
			quality = (int)val;
			break;
		}
		case 'm': {
			char *end;
			errno = 0;
			unsigned long long val = strtoull(optarg, &end, 10);
			if (errno != 0 || end == optarg || *end != '\0' || optarg[0] == '-' || val > SIZE_MAX) {
				PRINTF_ERR("Invalid max-pixels: %s\n", optarg);
				return EXIT_FAILURE;
			}
			max_pixels = (size_t)val;
			break;
		}
		case 'B': {
			char *end;
			errno = 0;
			unsigned long long val = strtoull(optarg, &end, 10);
			if (errno != 0 || end == optarg || *end != '\0' || optarg[0] == '-' || val > SIZE_MAX) {
				PRINTF_ERR("Invalid max-bytes: %s\n", optarg);
				return EXIT_FAILURE;
			}
			max_bytes = (size_t)val;
			break;
		}
		case 'o':
			output_path = optarg;
			break;
		case 'h':
			PUTS(
				"Usage: img-converter [OPTIONS] INPUT\n"
				"\n"
				"Convert images between formats.\n"
				"\n"
				"Options:\n"
				"  -f, --format FORMAT   Output format\n"
				"  -q, --quality N       Lossy quality 1-100 (default: 85)\n"
				"  -o, --output FILE     Output file (required)\n"
				"  -m, --max-pixels N    Fail if width*height > N (0 = unlimited)\n"
				"  -B, --max-bytes N     Fail if input file size > N (0 = unlimited)\n"
				"  -h, --help            Show this help\n"
				"\n"
				"Supported formats:\n"
				"  png          PNG (lossless, transparency)\n"
				"  jpg, jpeg    JPEG (lossy, no transparency)\n"
				"  bmp          BMP (lossless, no transparency)\n"
				"  qoi          QOI (lossless, transparency)\n"
			);
#ifdef HAVE_TIFF
			PUTS("  tiff, tif    TIFF (lossless, transparency)\n");
#else
			PUTS("  tiff, tif    TIFF (lossless, transparency) [requires libtiff]\n");
#endif
#ifdef HAVE_WEBP
			PUTS("  webp         WebP (lossy, transparency)\n");
#else
			PUTS("  webp         WebP (lossy, transparency) [requires libwebp]\n");
#endif
#ifdef HAVE_AVIF
			PUTS("  avif         AVIF (lossy, transparency)\n");
#else
			PUTS("  avif         AVIF (lossy, transparency) [requires libavif]\n");
#endif
#ifdef HAVE_HEIF
			PUTS("  heic, heif   HEIC (lossy, transparency)\n");
#else
			PUTS("  heic, heif   HEIC (lossy, transparency) [requires libheif]\n");
#endif
#ifdef HAVE_JXL
			PUTS("  jxl          JPEG XL (lossy/lossless, transparency)\n");
#else
			PUTS("  jxl          JPEG XL (lossy/lossless, transparency) [requires libjxl]\n");
#endif
			PUTS(
				"\n"
				"Formats are auto-detected from file extension if not specified."
			);
			return EXIT_SUCCESS;
		case '?':
			return EXIT_FAILURE;
		}
	}

	if (optind >= argc) {
		PUTS_ERR("Error: no input file specified\n");
		return EXIT_FAILURE;
	}
	if (optind + 1 < argc) {
		PUTS_ERR("Error: too many input files\n");
		return EXIT_FAILURE;
	}

	if (!output_path) {
		PUTS_ERR("Error: output file required (-o)\n");
		return EXIT_FAILURE;
	}

	const char *input_path = argv[optind];

	if (max_bytes != 0) {
		struct stat st;
		if (stat(input_path, &st) == 0 && S_ISREG(st.st_mode)) {
			if (st.st_size < 0 || (uintmax_t)st.st_size > (uintmax_t)max_bytes) {
				PRINTF_ERR("Error: input file exceeds --max-bytes limit (%zu bytes)\n", max_bytes);
				return EXIT_FAILURE;
			}
		}
	}

	// Detect formats
	enum format from_fmt = detect_format(input_path);
	if (from_fmt == FMT_UNKNOWN) {
		PUTS_ERR("Error: cannot detect input format\n");
		return EXIT_FAILURE;
	}

	if (to_fmt == FMT_UNKNOWN) {
		to_fmt = detect_format(output_path);
		if (to_fmt == FMT_UNKNOWN) {
			PUTS_ERR("Error: cannot detect output format, use -f\n");
			return EXIT_FAILURE;
		}
	}

	// Read input
	struct image img = {0};
	int exit_code = EXIT_FAILURE;
	bool ok = false;
	bool have_pixels = false;
	int read_errno = 0;
	errno = 0;

	switch (from_fmt) {
		case FMT_PNG:
			ok = png_read(input_path, &img);
			break;
		case FMT_JPEG:
			ok = jpeg_read(input_path, &img);
			break;
		case FMT_BMP:
			ok = bmp_read(input_path, &img);
			break;
		case FMT_QOI:
			ok = qoi_read(input_path, &img);
			break;
#ifdef HAVE_TIFF
		case FMT_TIFF:
			ok = tiff_read(input_path, &img);
			break;
#endif
#ifdef HAVE_WEBP
		case FMT_WEBP:
			ok = webp_read(input_path, &img);
			break;
#endif
#ifdef HAVE_AVIF
		case FMT_AVIF:
			ok = avif_read(input_path, &img);
			break;
#endif
#ifdef HAVE_HEIF
		case FMT_HEIF:
			ok = heif_read(input_path, &img);
			break;
#endif
#ifdef HAVE_JXL
		case FMT_JXL:
			ok = jxl_read(input_path, &img);
			break;
#endif
		default:
			break;
	}
	read_errno = errno;
	if (ok)
		have_pixels = true;

	if (!ok) {
		if (read_errno == EFBIG) {
			PRINTF_ERR("Error: input file exceeds --max-bytes limit (%zu bytes)\n", max_bytes);
			goto cleanup;
		}
		PRINTF_ERR("Error: failed to read %s\n", input_path);
		goto cleanup;
	}

	// Write output
	switch (to_fmt) {
		case FMT_PNG:
			ok = png_write(output_path, &img);
			break;
		case FMT_JPEG:
			ok = jpeg_write(output_path, &img, quality);
			break;
		case FMT_BMP:
			ok = bmp_write(output_path, &img);
			break;
		case FMT_QOI:
			ok = qoi_write(output_path, &img);
			break;
#ifdef HAVE_TIFF
		case FMT_TIFF:
			ok = tiff_write(output_path, &img);
			break;
#endif
#ifdef HAVE_WEBP
		case FMT_WEBP:
			ok = webp_write(output_path, &img, quality);
			break;
#endif
#ifdef HAVE_AVIF
		case FMT_AVIF:
			ok = avif_write(output_path, &img, quality);
			break;
#endif
#ifdef HAVE_HEIF
		case FMT_HEIF:
			ok = heif_write(output_path, &img, quality);
			break;
#endif
#ifdef HAVE_JXL
		case FMT_JXL:
			ok = jxl_write(output_path, &img, quality);
			break;
#endif
		default:
			break;
	}

	if (!ok) {
		PRINTF_ERR("Error: failed to write %s\n", output_path);
		goto cleanup;
	}

	exit_code = EXIT_SUCCESS;

cleanup:
	if (have_pixels)
		free(img.pixels);
	return exit_code;
}
