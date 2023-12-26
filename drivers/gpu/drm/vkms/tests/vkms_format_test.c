// SPDX-License-Identifier: GPL-2.0+

#include <kunit/test.h>

#include <drm/drm_fixed.h>

#include "../../drm_crtc_internal.h"

#define TEST_BUFF_SIZE 50

struct yuv_u8_to_argb_u16_case {
	enum drm_color_encoding encoding;
	enum drm_color_range range;
	size_t n_colors;
	struct format_pair {
		char *name;
		struct pixel_yuv_u8 yuv;
		struct pixel_argb_u16 argb;
	} colors[TEST_BUFF_SIZE];
};

static struct yuv_u8_to_argb_u16_case yuv_u8_to_argb_u16_cases[] = {
	{
		.encoding = DRM_COLOR_YCBCR_BT601,
		.range = DRM_COLOR_YCBCR_FULL_RANGE,
		.n_colors = 6,
		.colors = {
			{"white", {0xff, 0x80, 0x80}, {0x0000, 0xffff, 0xffff, 0xffff}},
			{"gray",  {0x80, 0x80, 0x80}, {0x0000, 0x8000, 0x8000, 0x8000}},
			{"black", {0x00, 0x80, 0x80}, {0x0000, 0x0000, 0x0000, 0x0000}},
			{"red",   {0x4c, 0x55, 0xff}, {0x0000, 0xffff, 0x0000, 0x0000}},
			{"green", {0x96, 0x2c, 0x15}, {0x0000, 0x0000, 0xffff, 0x0000}},
			{"blue",  {0x1d, 0xff, 0x6b}, {0x0000, 0x0000, 0x0000, 0xffff}},
		},
	},
	{
		.encoding = DRM_COLOR_YCBCR_BT601,
		.range = DRM_COLOR_YCBCR_LIMITED_RANGE,
		.n_colors = 6,
		.colors = {
			{"white", {0xeb, 0x80, 0x80}, {0x0000, 0xffff, 0xffff, 0xffff}},
			{"gray",  {0x7e, 0x80, 0x80}, {0x0000, 0x8000, 0x8000, 0x8000}},
			{"black", {0x10, 0x80, 0x80}, {0x0000, 0x0000, 0x0000, 0x0000}},
			{"red",   {0x51, 0x5a, 0xf0}, {0x0000, 0xffff, 0x0000, 0x0000}},
			{"green", {0x91, 0x36, 0x22}, {0x0000, 0x0000, 0xffff, 0x0000}},
			{"blue",  {0x29, 0xf0, 0x6e}, {0x0000, 0x0000, 0x0000, 0xffff}},
		},
	},
	{
		.encoding = DRM_COLOR_YCBCR_BT709,
		.range = DRM_COLOR_YCBCR_FULL_RANGE,
		.n_colors = 4,
		.colors = {
			{"white", {0xff, 0x80, 0x80}, {0x0000, 0xffff, 0xffff, 0xffff}},
			{"gray",  {0x80, 0x80, 0x80}, {0x0000, 0x8000, 0x8000, 0x8000}},
			{"black", {0x00, 0x80, 0x80}, {0x0000, 0x0000, 0x0000, 0x0000}},
			{"red",   {0x35, 0x63, 0xff}, {0x0000, 0xffff, 0x0000, 0x0000}},
			{"green", {0xb6, 0x1e, 0x0c}, {0x0000, 0x0000, 0xffff, 0x0000}},
			{"blue",  {0x12, 0xff, 0x74}, {0x0000, 0x0000, 0x0000, 0xffff}},
		},
	},
	{
		.encoding = DRM_COLOR_YCBCR_BT709,
		.range = DRM_COLOR_YCBCR_LIMITED_RANGE,
		.n_colors = 4,
		.colors = {
			{"white", {0xeb, 0x80, 0x80}, {0x0000, 0xffff, 0xffff, 0xffff}},
			{"gray",  {0x7e, 0x80, 0x80}, {0x0000, 0x8000, 0x8000, 0x8000}},
			{"black", {0x10, 0x80, 0x80}, {0x0000, 0x0000, 0x0000, 0x0000}},
			{"red",   {0x3f, 0x66, 0xf0}, {0x0000, 0xffff, 0x0000, 0x0000}},
			{"green", {0xad, 0x2a, 0x1a}, {0x0000, 0x0000, 0xffff, 0x0000}},
			{"blue",  {0x20, 0xf0, 0x76}, {0x0000, 0x0000, 0x0000, 0xffff}},
		},
	},
	{
		.encoding = DRM_COLOR_YCBCR_BT2020,
		.range = DRM_COLOR_YCBCR_FULL_RANGE,
		.n_colors = 4,
		.colors = {
			{"white", {0xff, 0x80, 0x80}, {0x0000, 0xffff, 0xffff, 0xffff}},
			{"gray",  {0x80, 0x80, 0x80}, {0x0000, 0x8000, 0x8000, 0x8000}},
			{"black", {0x00, 0x80, 0x80}, {0x0000, 0x0000, 0x0000, 0x0000}},
			{"red",   {0x43, 0x5c, 0xff}, {0x0000, 0xffff, 0x0000, 0x0000}},
			{"green", {0xad, 0x24, 0x0b}, {0x0000, 0x0000, 0xffff, 0x0000}},
			{"blue",  {0x0f, 0xff, 0x76}, {0x0000, 0x0000, 0x0000, 0xffff}},
		},
	},
	{
		.encoding = DRM_COLOR_YCBCR_BT2020,
		.range = DRM_COLOR_YCBCR_LIMITED_RANGE,
		.n_colors = 4,
		.colors = {
			{"white", {0xeb, 0x80, 0x80}, {0x0000, 0xffff, 0xffff, 0xffff}},
			{"gray",  {0x7e, 0x80, 0x80}, {0x0000, 0x8000, 0x8000, 0x8000}},
			{"black", {0x10, 0x80, 0x80}, {0x0000, 0x0000, 0x0000, 0x0000}},
			{"red",   {0x4a, 0x61, 0xf0}, {0x0000, 0xffff, 0x0000, 0x0000}},
			{"green", {0xa4, 0x2f, 0x19}, {0x0000, 0x0000, 0xffff, 0x0000}},
			{"blue",  {0x1d, 0xf0, 0x77}, {0x0000, 0x0000, 0x0000, 0xffff}},
		},
	},
};

static void vkms_format_test_yuv_u8_to_argb_u16(struct kunit *test)
{
	const struct yuv_u8_to_argb_u16_case *param = test->param_value;
	struct pixel_argb_u16 argb;

	for (size_t i = 0; i < param->n_colors; i++) {
		const struct format_pair *color = &param->colors[i];

		yuv_u8_to_argb_u16(&argb, &color->yuv, param->encoding, param->range);

		KUNIT_EXPECT_LE_MSG(test, abs_diff(argb.a, color->argb.a), 257,
				    "On the A channel of the color %s expected 0x%04x, got 0x%04x",
				    color->name, color->argb.a, argb.a);
		KUNIT_EXPECT_LE_MSG(test, abs_diff(argb.r, color->argb.r), 257,
				    "On the R channel of the color %s expected 0x%04x, got 0x%04x",
				    color->name, color->argb.r, argb.r);
		KUNIT_EXPECT_LE_MSG(test, abs_diff(argb.g, color->argb.g), 257,
				    "On the G channel of the color %s expected 0x%04x, got 0x%04x",
				    color->name, color->argb.g, argb.g);
		KUNIT_EXPECT_LE_MSG(test, abs_diff(argb.b, color->argb.b), 257,
				    "On the B channel of the color %s expected 0x%04x, got 0x%04x",
				    color->name, color->argb.b, argb.b);
	}
}


static void vkms_format_test_yuv_u8_to_argb_u16_case_desc(struct yuv_u8_to_argb_u16_case *t,
							  char *desc)
{
	snprintf(desc, KUNIT_PARAM_DESC_SIZE, "%s - %s",
		 drm_get_color_encoding_name(t->encoding), drm_get_color_range_name(t->range));
}

KUNIT_ARRAY_PARAM(yuv_u8_to_argb_u16, yuv_u8_to_argb_u16_cases,
		  vkms_format_test_yuv_u8_to_argb_u16_case_desc);

static struct kunit_case vkms_format_test_cases[] = {
	KUNIT_CASE_PARAM(vkms_format_test_yuv_u8_to_argb_u16, yuv_u8_to_argb_u16_gen_params),
	{}
};

static struct kunit_suite vkms_format_test_suite = {
	.name = "vkms-format",
	.test_cases = vkms_format_test_cases,
};

kunit_test_suite(vkms_format_test_suite);

MODULE_LICENSE("GPL");
