// SPDX-License-Identifier: GPL-2.0+

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
 * returns the address of the first color channel on the desired index. The
 * format's specific subsample is applied.
 */
static void *packed_pixels_addr(const struct vkms_frame_info *frame_info,
				int x, int y, size_t index)
{
	int vsub = index == 0 ? 1 : frame_info->fb->format->vsub;
	int hsub = index == 0 ? 1 : frame_info->fb->format->hsub;
	size_t offset = pixel_offset(frame_info, x / hsub, y / vsub, index);

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

static void ycbcr2rgb(const s16 m[3][3], u8 y, u8 cb, u8 cr, u8 y_offset, u8 *r, u8 *g, u8 *b)
{
	s32 y_16, cb_16, cr_16;
	s32 r_16, g_16, b_16;

	y_16 =  y - y_offset;
	cb_16 = cb - 128;
	cr_16 = cr - 128;

	r_16 = m[0][0] * y_16 + m[0][1] * cb_16 + m[0][2] * cr_16;
	g_16 = m[1][0] * y_16 + m[1][1] * cb_16 + m[1][2] * cr_16;
	b_16 = m[2][0] * y_16 + m[2][1] * cb_16 + m[2][2] * cr_16;

	*r = clamp(r_16, 0, 0xffff) >> 8;
	*g = clamp(g_16, 0, 0xffff) >> 8;
	*b = clamp(b_16, 0, 0xffff) >> 8;
}

static void yuv_u8_to_argb_u16(struct pixel_argb_u16 *argb_u16, const struct pixel_yuv_u8 *yuv_u8,
			       enum drm_color_encoding encoding, enum drm_color_range range)
{
	static const s16 bt601_full[3][3] = {
		{256,   0,  359},
		{256, -88, -183},
		{256, 454,    0},
	};
	static const s16 bt601[3][3] = {
		{298,    0,  409},
		{298, -100, -208},
		{298,  516,    0},
	};
	static const s16 rec709_full[3][3] = {
		{256,   0,  408},
		{256, -48, -120},
		{256, 476,   0 },
	};
	static const s16 rec709[3][3] = {
		{298,   0,  459},
		{298, -55, -136},
		{298, 541,    0},
	};
	static const s16 bt2020_full[3][3] = {
		{256,   0,  377},
		{256, -42, -146},
		{256, 482,    0},
	};
	static const s16 bt2020[3][3] = {
		{298,   0,  430},
		{298, -48, -167},
		{298, 548,    0},
	};

	u8 r = 0;
	u8 g = 0;
	u8 b = 0;
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

	argb_u16->r = r * 257;
	argb_u16->g = g * 257;
	argb_u16->b = b * 257;
}

static void semi_planar_yuv_to_argb_u16(u8 **src_pixels, struct pixel_argb_u16 *out_pixel,
					enum drm_color_encoding encoding,
					enum drm_color_range range)
{
	struct pixel_yuv_u8 yuv_u8;

	yuv_u8.y = src_pixels[0][0];
	yuv_u8.u = src_pixels[1][0];
	yuv_u8.v = src_pixels[1][1];

	yuv_u8_to_argb_u16(out_pixel, &yuv_u8, encoding, range);
}

static void semi_planar_yvu_to_argb_u16(u8 **src_pixels, struct pixel_argb_u16 *out_pixel,
					enum drm_color_encoding encoding,
					enum drm_color_range range)
{
	struct pixel_yuv_u8 yuv_u8;

	yuv_u8.y = src_pixels[0][0];
	yuv_u8.v = src_pixels[1][0];
	yuv_u8.u = src_pixels[1][1];

	yuv_u8_to_argb_u16(out_pixel, &yuv_u8, encoding, range);
}

static void planar_yuv_to_argb_u16(u8 **src_pixels, struct pixel_argb_u16 *out_pixel,
				   enum drm_color_encoding encoding, enum drm_color_range range)
{
	struct pixel_yuv_u8 yuv_u8;

	yuv_u8.y = src_pixels[0][0];
	yuv_u8.u = src_pixels[1][0];
	yuv_u8.v = src_pixels[2][0];

	yuv_u8_to_argb_u16(out_pixel, &yuv_u8, encoding, range);
}

static void planar_yvu_to_argb_u16(u8 **src_pixels, struct pixel_argb_u16 *out_pixel,
				   enum drm_color_encoding encoding, enum drm_color_range range)
{
	struct pixel_yuv_u8 yuv_u8;

	yuv_u8.y = src_pixels[0][0];
	yuv_u8.v = src_pixels[1][0];
	yuv_u8.u = src_pixels[2][0];

	yuv_u8_to_argb_u16(out_pixel, &yuv_u8, encoding, range);
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

	for (size_t i = 0; i < frame_format->num_planes; i++)
		src_pixels[i] = get_packed_src_addr(frame_info, y, i);

	for (size_t x = 0; x < limit; x++) {
		int x_pos = get_x_position(frame_info, limit, x);

		bool shoud_inc = !((x + 1) % frame_format->num_planes);

		if (drm_rotation_90_or_270(frame_info->rotation)) {
			for (size_t i = 0; i < frame_format->num_planes; i++) {
				src_pixels[i] = get_packed_src_addr(frame_info,
								    x + frame_info->rotated.y1, i);
				if (!i || shoud_inc)
					src_pixels[i] += frame_format->cpp[i] * y;
			}
		}

		plane->pixel_read(src_pixels, &out_pixels[x_pos], encoding, range);

		for (size_t i = 0; i < frame_format->num_planes; i++) {
			if (!i || shoud_inc)
				src_pixels[i] += frame_format->cpp[i];
		}
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
	case DRM_FORMAT_NV16:
	case DRM_FORMAT_NV24:
		return &semi_planar_yuv_to_argb_u16;
	case DRM_FORMAT_NV21:
	case DRM_FORMAT_NV61:
	case DRM_FORMAT_NV42:
		return &semi_planar_yvu_to_argb_u16;
	case DRM_FORMAT_YUV420:
	case DRM_FORMAT_YUV422:
	case DRM_FORMAT_YUV444:
		return &planar_yuv_to_argb_u16;
	case DRM_FORMAT_YVU420:
	case DRM_FORMAT_YVU422:
	case DRM_FORMAT_YVU444:
		return &planar_yvu_to_argb_u16;
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
	default:
		return NULL;
	}
}
