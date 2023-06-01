// SPDX-License-Identifier: GPL-2.0+

#include "vkms_drv.h"
#include "drm/drm_color_mgmt.h"
#include <linux/kernel.h>
#include <linux/minmax.h>

#include <drm/drm_blend.h>
#include <drm/drm_rect.h>
#include <drm/drm_fixed.h>

#include "vkms_formats.h"

static size_t pixel_offset(const struct vkms_frame_info *frame_info, int x, int y, size_t index)
{
	struct drm_framebuffer *fb = frame_info->fb;

	return fb->offsets[index] + (y * fb->pitches[index])
				  + (x * fb->format->cpp[index]);
}

/*
 * packed_pixels_addr - Get the pointer to pixel of a given pair of coordinates
 *
 * @frame_info: Buffer metadata
 * @x: The x(width) coordinate of the 2D buffer
 * @y: The y(Heigth) coordinate of the 2D buffer
 * @index: The index of the plane on the 2D buffer
 *
 * Takes the information stored in the frame_info, a pair of coordinates, and
 * returns the address of the first color channel on the desired index.
 */
static void *packed_pixels_addr(const struct vkms_frame_info *frame_info,
				int x, int y, size_t index)
{
	size_t offset = pixel_offset(frame_info, x, y, index);

	return (u8 *)frame_info->map[0].vaddr + offset;
}

static void *get_packed_src_addr(const struct vkms_frame_info *frame_info, int y, size_t index)
{
	int x_src = frame_info->src.x1 >> 16;
	int y_src = y - frame_info->rotated.y1 + (frame_info->src.y1 >> 16);

	return packed_pixels_addr(frame_info, x_src, y_src, index);
}

static int get_x_position(const struct vkms_frame_info *frame_info, int limit, int x)
{
	if (frame_info->rotation & (DRM_MODE_REFLECT_X | DRM_MODE_ROTATE_270))
		return limit - x - 1;
	return x;
}

static void ARGB8888_to_argb_u16(u8 **src_pixels, struct pixel_argb_u16 *out_pixel,
				 enum drm_color_encoding encoding, enum drm_color_range range)
{
	/*
	 * The 257 is the "conversion ratio". This number is obtained by the
	 * (2^16 - 1) / (2^8 - 1) division. Which, in this case, tries to get
	 * the best color value in a pixel format with more possibilities.
	 * A similar idea applies to others RGB color conversions.
	 */
	out_pixel->a = (u16)src_pixels[0][3] * 257;
	out_pixel->r = (u16)src_pixels[0][2] * 257;
	out_pixel->g = (u16)src_pixels[0][1] * 257;
	out_pixel->b = (u16)src_pixels[0][0] * 257;
}

static void XRGB8888_to_argb_u16(u8 **src_pixels, struct pixel_argb_u16 *out_pixel,
				 enum drm_color_encoding encoding, enum drm_color_range range)
{
	out_pixel->a = (u16)0xffff;
	out_pixel->r = (u16)src_pixels[0][2] * 257;
	out_pixel->g = (u16)src_pixels[0][1] * 257;
	out_pixel->b = (u16)src_pixels[0][0] * 257;
}

static void ARGB16161616_to_argb_u16(u8 **src_pixels, struct pixel_argb_u16 *out_pixel,
				     enum drm_color_encoding encoding, enum drm_color_range range)
{
	u16 *pixels = (u16 *)src_pixels[0];

	out_pixel->a = le16_to_cpu(pixels[3]);
	out_pixel->r = le16_to_cpu(pixels[2]);
	out_pixel->g = le16_to_cpu(pixels[1]);
	out_pixel->b = le16_to_cpu(pixels[0]);
}

static void XRGB16161616_to_argb_u16(u8 **src_pixels, struct pixel_argb_u16 *out_pixel,
				     enum drm_color_encoding encoding, enum drm_color_range range)
{
	u16 *pixels = (u16 *)src_pixels[0];

	out_pixel->a = (u16)0xffff;
	out_pixel->r = le16_to_cpu(pixels[2]);
	out_pixel->g = le16_to_cpu(pixels[1]);
	out_pixel->b = le16_to_cpu(pixels[0]);
}

static void RGB565_to_argb_u16(u8 **src_pixels, struct pixel_argb_u16 *out_pixel,
			       enum drm_color_encoding encoding, enum drm_color_range range)
{
	u16 *pixels = (u16 *)src_pixels[0];

	s64 fp_rb_ratio = drm_fixp_div(drm_int2fixp(65535), drm_int2fixp(31));
	s64 fp_g_ratio = drm_fixp_div(drm_int2fixp(65535), drm_int2fixp(63));

	u16 rgb_565 = le16_to_cpu(*pixels);
	s64 fp_r = drm_int2fixp((rgb_565 >> 11) & 0x1f);
	s64 fp_g = drm_int2fixp((rgb_565 >> 5) & 0x3f);
	s64 fp_b = drm_int2fixp(rgb_565 & 0x1f);

	out_pixel->a = (u16)0xffff;
	out_pixel->r = drm_fixp2int_round(drm_fixp_mul(fp_r, fp_rb_ratio));
	out_pixel->g = drm_fixp2int_round(drm_fixp_mul(fp_g, fp_g_ratio));
	out_pixel->b = drm_fixp2int_round(drm_fixp_mul(fp_b, fp_rb_ratio));
}

struct pixel_yuv_u8 {
	u8 y, u, v;
};

static void ycbcr2rgb(const s64 m[3][3], int y, int cb, int cr,
		      int y_offset, int *r, int *g, int *b)
{
	s64 fp_y; s64 fp_cb; s64 fp_cr;
	s64 fp_r; s64 fp_g; s64 fp_b;

	y -= y_offset;
	cb -= 128;
	cr -= 128;

	fp_y = drm_int2fixp(y);
	fp_cb = drm_int2fixp(cb);
	fp_cr = drm_int2fixp(cr);

	fp_r = drm_fixp_mul(m[0][0], fp_y) +
	       drm_fixp_mul(m[0][1], fp_cb) +
	       drm_fixp_mul(m[0][2], fp_cr);

	fp_g = drm_fixp_mul(m[1][0], fp_y) +
	       drm_fixp_mul(m[1][1], fp_cb) +
	       drm_fixp_mul(m[1][2], fp_cr);

	fp_b = drm_fixp_mul(m[2][0], fp_y) +
	       drm_fixp_mul(m[2][1], fp_cb) +
	       drm_fixp_mul(m[2][2], fp_cr);

	*r = drm_fixp2int(fp_r);
	*g = drm_fixp2int(fp_g);
	*b = drm_fixp2int(fp_b);
}

static void yuv_u8_to_argb_u16(struct pixel_argb_u16 *argb_u16, struct pixel_yuv_u8 *yuv_u8,
			       enum drm_color_encoding encoding, enum drm_color_range range)
{
#define COEFF(v, r) (\
	drm_fixp_div(drm_fixp_mul(drm_fixp_from_fraction(v, 10000), drm_int2fixp((1 << 16) - 1)),\
		     drm_int2fixp(r)) \
	)\

	const s64 bt601[3][3] = {
		{ COEFF(10000, 219), COEFF(0, 224),     COEFF(14020, 224) },
		{ COEFF(10000, 219), COEFF(-3441, 224), COEFF(-7141, 224) },
		{ COEFF(10000, 219), COEFF(17720, 224), COEFF(0, 224)     },
	};
	const s64 bt601_full[3][3] = {
		{ COEFF(10000, 255), COEFF(0, 255),     COEFF(14020, 255) },
		{ COEFF(10000, 255), COEFF(-3441, 255), COEFF(-7141, 255) },
		{ COEFF(10000, 255), COEFF(17720, 255), COEFF(0, 255)     },
	};
	const s64 rec709[3][3] = {
		{ COEFF(10000, 219), COEFF(0, 224),     COEFF(15748, 224) },
		{ COEFF(10000, 219), COEFF(-1873, 224), COEFF(-4681, 224) },
		{ COEFF(10000, 219), COEFF(18556, 224), COEFF(0, 224)     },
	};
	const s64 rec709_full[3][3] = {
		{ COEFF(10000, 255), COEFF(0, 255),     COEFF(15748, 255) },
		{ COEFF(10000, 255), COEFF(-1873, 255), COEFF(-4681, 255) },
		{ COEFF(10000, 255), COEFF(18556, 255), COEFF(0, 255)     },
	};
	const s64 bt2020[3][3] = {
		{ COEFF(10000, 219), COEFF(0, 224),     COEFF(14746, 224) },
		{ COEFF(10000, 219), COEFF(-1646, 224), COEFF(-5714, 224) },
		{ COEFF(10000, 219), COEFF(18814, 224), COEFF(0, 224)     },
	};
	const s64 bt2020_full[3][3] = {
		{ COEFF(10000, 255), COEFF(0, 255),     COEFF(14746, 255) },
		{ COEFF(10000, 255), COEFF(-1646, 255), COEFF(-5714, 255) },
		{ COEFF(10000, 255), COEFF(18814, 255), COEFF(0, 255)     },
	};

	int r = 0;
	int g = 0;
	int b = 0;
	bool full = range == DRM_COLOR_YCBCR_FULL_RANGE;
	unsigned int y_offset = full ? 0 : 16;

	switch (encoding) {
	case DRM_COLOR_YCBCR_BT601:
		ycbcr2rgb(full ? bt601_full : bt601,
			  yuv_u8->y, yuv_u8->u, yuv_u8->v, y_offset, &r, &g, &b);
		break;
	case DRM_COLOR_YCBCR_BT709:
		ycbcr2rgb(full ? rec709_full : rec709,
			  yuv_u8->y, yuv_u8->u, yuv_u8->v, y_offset, &r, &g, &b);
		break;
	case DRM_COLOR_YCBCR_BT2020:
		ycbcr2rgb(full ? bt2020_full : bt2020,
			  yuv_u8->y, yuv_u8->u, yuv_u8->v, y_offset, &r, &g, &b);
		break;
	default:
		pr_warn_once("Not supported color encoding\n");
		break;
	}

	argb_u16->r = clamp(r, 0, 0xffff);
	argb_u16->g = clamp(g, 0, 0xffff);
	argb_u16->b = clamp(b, 0, 0xffff);
}

static void NV12_to_argb_u16(u8 **src_pixels, struct pixel_argb_u16 *out_pixel,
			     enum drm_color_encoding encoding, enum drm_color_range range)
{
	struct pixel_yuv_u8 yuv_u8;

	yuv_u8.y = src_pixels[0][0];
	yuv_u8.u = src_pixels[1][0];
	yuv_u8.v = src_pixels[1][1];

	yuv_u8_to_argb_u16(out_pixel, &yuv_u8, encoding, range);
}

static void get_src_pixels_per_plane(const struct vkms_frame_info *frame_info,
				     u8 **src_pixels, size_t y)
{
	const struct drm_format_info *frame_format = frame_info->fb->format;

	for (size_t i = 0; i < frame_format->num_planes; i++)
		src_pixels[i] = get_packed_src_addr(frame_info, y, i);
}


/**
 * vkms_compose_row - compose a single row of a plane
 * @stage_buffer: output line with the composed pixels
 * @plane: state of the plane that is being composed
 * @y: y coordinate of the row
 *
 * This function composes a single row of a plane. It gets the source pixels
 * through the y coordinate (see get_packed_src_addr()) and goes linearly
 * through the source pixel, reading the pixels and converting it to
 * ARGB16161616 (see the pixel_read() callback). For rotate-90 and rotate-270,
 * the source pixels are not traversed linearly. The source pixels are queried
 * on each iteration in order to traverse the pixels vertically.
 */
void vkms_compose_row(struct line_buffer *stage_buffer, struct vkms_plane_state *plane, int y)
{
	struct pixel_argb_u16 *out_pixels = stage_buffer->pixels;
	struct vkms_frame_info *frame_info = plane->frame_info;
	const struct drm_format_info *frame_format = frame_info->fb->format;
	int limit = min_t(size_t, drm_rect_width(&frame_info->dst), stage_buffer->n_pixels);
	u8 *src_pixels[DRM_FORMAT_MAX_PLANES];

	enum drm_color_encoding encoding = plane->base.base.color_encoding;
	enum drm_color_range range = plane->base.base.color_range;

	get_src_pixels_per_plane(frame_info, src_pixels, y);

	for (size_t x = 0; x < limit; x++) {
		int x_pos = get_x_position(frame_info, limit, x);

		if (drm_rotation_90_or_270(frame_info->rotation)) {
			for (size_t i = 0; i < frame_format->num_planes; i++)
				src_pixels[i] = get_packed_src_addr(frame_info,
								    x + frame_info->rotated.y1, i) +
								    frame_format->cpp[i] * y;
		}

		plane->pixel_read(src_pixels, &out_pixels[x_pos], encoding, range);

		for (size_t i = 0; i < frame_format->num_planes; i++)
			src_pixels[i] += frame_format->cpp[i];
	}
}

/*
 * The following  functions take an line of argb_u16 pixels from the
 * src_buffer, convert them to a specific format, and store them in the
 * destination.
 *
 * They are used in the `compose_active_planes` to convert and store a line
 * from the src_buffer to the writeback buffer.
 */
static void argb_u16_to_ARGB8888(u8 *dst_pixels, struct pixel_argb_u16 *in_pixel)
{
	/*
	 * This sequence below is important because the format's byte order is
	 * in little-endian. In the case of the ARGB8888 the memory is
	 * organized this way:
	 *
	 * | Addr     | = blue channel
	 * | Addr + 1 | = green channel
	 * | Addr + 2 | = Red channel
	 * | Addr + 3 | = Alpha channel
	 */
	dst_pixels[3] = DIV_ROUND_CLOSEST(in_pixel->a, 257);
	dst_pixels[2] = DIV_ROUND_CLOSEST(in_pixel->r, 257);
	dst_pixels[1] = DIV_ROUND_CLOSEST(in_pixel->g, 257);
	dst_pixels[0] = DIV_ROUND_CLOSEST(in_pixel->b, 257);
}

static void argb_u16_to_XRGB8888(u8 *dst_pixels, struct pixel_argb_u16 *in_pixel)
{
	dst_pixels[3] = 0xff;
	dst_pixels[2] = DIV_ROUND_CLOSEST(in_pixel->r, 257);
	dst_pixels[1] = DIV_ROUND_CLOSEST(in_pixel->g, 257);
	dst_pixels[0] = DIV_ROUND_CLOSEST(in_pixel->b, 257);
}

static void argb_u16_to_ARGB16161616(u8 *dst_pixels, struct pixel_argb_u16 *in_pixel)
{
	u16 *pixels = (u16 *)dst_pixels;

	pixels[3] = cpu_to_le16(in_pixel->a);
	pixels[2] = cpu_to_le16(in_pixel->r);
	pixels[1] = cpu_to_le16(in_pixel->g);
	pixels[0] = cpu_to_le16(in_pixel->b);
}

static void argb_u16_to_XRGB16161616(u8 *dst_pixels, struct pixel_argb_u16 *in_pixel)
{
	u16 *pixels = (u16 *)dst_pixels;

	pixels[3] = 0xffff;
	pixels[2] = cpu_to_le16(in_pixel->r);
	pixels[1] = cpu_to_le16(in_pixel->g);
	pixels[0] = cpu_to_le16(in_pixel->b);
}

static void argb_u16_to_RGB565(u8 *dst_pixels, struct pixel_argb_u16 *in_pixel)
{
	u16 *pixels = (u16 *)dst_pixels;

	s64 fp_rb_ratio = drm_fixp_div(drm_int2fixp(65535), drm_int2fixp(31));
	s64 fp_g_ratio = drm_fixp_div(drm_int2fixp(65535), drm_int2fixp(63));

	s64 fp_r = drm_int2fixp(in_pixel->r);
	s64 fp_g = drm_int2fixp(in_pixel->g);
	s64 fp_b = drm_int2fixp(in_pixel->b);

	u16 r = drm_fixp2int(drm_fixp_div(fp_r, fp_rb_ratio));
	u16 g = drm_fixp2int(drm_fixp_div(fp_g, fp_g_ratio));
	u16 b = drm_fixp2int(drm_fixp_div(fp_b, fp_rb_ratio));

	*pixels = cpu_to_le16(r << 11 | g << 5 | b);
}

void vkms_writeback_row(struct vkms_writeback_job *wb,
			const struct line_buffer *src_buffer, int y)
{
	struct vkms_frame_info *frame_info = &wb->wb_frame_info;
	int x_dst = frame_info->dst.x1;
	u8 *dst_pixels = packed_pixels_addr(frame_info, x_dst, y, 0);
	struct pixel_argb_u16 *in_pixels = src_buffer->pixels;
	int x_limit = min_t(size_t, drm_rect_width(&frame_info->dst), src_buffer->n_pixels);

	for (size_t x = 0; x < x_limit; x++, dst_pixels += frame_info->fb->format->cpp[0])
		wb->pixel_write(dst_pixels, &in_pixels[x]);
}

/*
 * The conversion was based on the article below:
 * https://learn.microsoft.com/en-us/windows/win32/medfound/recommended-8-bit-yuv-formats-for-video-rendering#converting-rgb888-to-yuv-444
 */
static void argb_u16_to_yuv_u8(struct pixel_yuv_u8 *yuv_u8, const struct pixel_argb_u16 *argb_u16)
{
	u8 r_u8 = DIV_ROUND_CLOSEST(argb_u16->r, 257);
	u8 g_u8 = DIV_ROUND_CLOSEST(argb_u16->g, 257);
	u8 b_u8 = DIV_ROUND_CLOSEST(argb_u16->b, 257);

	yuv_u8->y = ((66 * r_u8  + 129 * g_u8 +  25 * b_u8 + 128) >> 8) +  16;
	yuv_u8->u = ((-38 * r_u8 -  74 * g_u8 + 112 * b_u8 + 128) >> 8) + 128;
	yuv_u8->v = ((112 * r_u8 -  94 * g_u8 -  18 * b_u8 + 128) >> 8) + 128;
}


static void argb_u16_to_NV12(struct vkms_frame_info *frame_info,
			     const struct line_buffer *src_buffer, int y)
{
	int x_dst = frame_info->dst.x1;
	u8 *dst_y = packed_pixels_addr(frame_info, x_dst, y, 0);
	u8 *dst_uv = packed_pixels_addr(frame_info, x_dst, y / 2, 1);
	struct pixel_yuv_u8 yuv_u8;
	struct pixel_argb_u16 *in_pixels = src_buffer->pixels;
	int x_limit = min_t(size_t, drm_rect_width(&frame_info->dst),
			    src_buffer->n_pixels);

	for (size_t x = 0; x < x_limit; x++) {
		argb_u16_to_yuv_u8(&yuv_u8, &in_pixels[x]);
		dst_y[x] = yuv_u8.y;
		dst_uv[x / 2 + x / 2] = yuv_u8.u;
		dst_uv[x / 2 + x / 2 + 1] = yuv_u8.v;
	}
}

void *get_pixel_conversion_function(u32 format)
{
	switch (format) {
	case DRM_FORMAT_ARGB8888:
		return &ARGB8888_to_argb_u16;
	case DRM_FORMAT_XRGB8888:
		return &XRGB8888_to_argb_u16;
	case DRM_FORMAT_ARGB16161616:
		return &ARGB16161616_to_argb_u16;
	case DRM_FORMAT_XRGB16161616:
		return &XRGB16161616_to_argb_u16;
	case DRM_FORMAT_RGB565:
		return &RGB565_to_argb_u16;
	case DRM_FORMAT_NV12:
		return &NV12_to_argb_u16;
	default:
		return NULL;
	}
}

void *get_pixel_write_function(u32 format)
{
	switch (format) {
	case DRM_FORMAT_ARGB8888:
		return &argb_u16_to_ARGB8888;
	case DRM_FORMAT_XRGB8888:
		return &argb_u16_to_XRGB8888;
	case DRM_FORMAT_ARGB16161616:
		return &argb_u16_to_ARGB16161616;
	case DRM_FORMAT_XRGB16161616:
		return &argb_u16_to_XRGB16161616;
	case DRM_FORMAT_RGB565:
		return &argb_u16_to_RGB565;
	case DRM_FORMAT_NV12:
		return &argb_u16_to_NV12;
	default:
		return NULL;
	}
}
