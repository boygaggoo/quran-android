#include <hb-ft.h>
#include <freetype/ftglyph.h>
#include <android/bitmap.h>

#include "render.h"

static char LOG_TAG[] = "NativeRenderer";

static int ft_ready = 0;
static FT_Library ft_library;
static FT_Face ft_face;

static jbyte *font_blob = 0;

extern hb_unicode_funcs_t *hb_ucdn_get_unicode_funcs(void);

static void render_bitmap(FT_Bitmap *bitmap, void *pixels, AndroidBitmapInfo *abi, int x, int y)
{
	int ix, iy, ox, oy;
	for (iy = 0; iy < bitmap->rows; iy++) {
		int oy = y + iy;
		if (oy < 0 || oy >= abi->height)
			continue;
		unsigned char *ipix = bitmap->buffer + iy * bitmap->width;
		unsigned char *opix = (unsigned char *) pixels + oy * abi->stride;
		for (ix = 0; ix < bitmap->width; ix++) {
			int ox = x + ix;
			if (ox < 0 || ox >= abi->width)
				continue;
			if (opix[ox] < ipix[ix])
				opix[ox] = ipix[ix];
		}
	}
}

static void render_glyphs(hb_buffer_t *buf, void *pixels, AndroidBitmapInfo *abi, int x, int y)
{
	FT_GlyphSlot slot = ft_face->glyph;
	int len = hb_buffer_get_length(buf);
	hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buf, 0);
	hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buf, 0);
	int i;

	x <<= 6;
	y <<= 6;
	for (i = 0; i < len; i++) {
		FT_Load_Glyph(ft_face, info->codepoint, FT_LOAD_DEFAULT);
		FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL);

		render_bitmap(&slot->bitmap, pixels, abi,
				(x + pos->x_offset >> 6) + slot->bitmap_left,
				(y - pos->y_offset >> 6) - slot->bitmap_top);

		x += pos->x_advance;
		y -= pos->y_advance;

		info++;
		pos++;
   	}
}

static void compute_bbox(hb_buffer_t *buf, FT_BBox *bbox)
{
	FT_GlyphSlot slot = ft_face->glyph;
	int len = hb_buffer_get_length(buf);
	hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buf, 0);
	hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buf, 0);
	int i;

	bbox->xMin = bbox->yMin = 32000;
	bbox->xMax = bbox->yMax = -32000;
	int x = 0;
	int y = 0;
	for (i = 0; i < len; i++) {
		FT_BBox glyph_bbox;
		FT_Glyph glyph;

		FT_Load_Glyph(ft_face, info->codepoint, FT_LOAD_DEFAULT);
		FT_Get_Glyph(slot, &glyph);
		FT_Glyph_Get_CBox(glyph, FT_GLYPH_BBOX_PIXELS, &glyph_bbox);
		FT_Done_Glyph(glyph);

		glyph_bbox.xMin <<= 6;
		glyph_bbox.xMax <<= 6;
		glyph_bbox.yMin <<= 6;
		glyph_bbox.yMax <<= 6;

		glyph_bbox.xMin += x + pos->x_offset;
		glyph_bbox.xMax += x + pos->x_offset;
		glyph_bbox.yMin += y + pos->y_offset;
		glyph_bbox.yMax += y + pos->y_offset;

		if (bbox->xMin > glyph_bbox.xMin)
			bbox->xMin = glyph_bbox.xMin;
		if (bbox->yMin > glyph_bbox.yMin)
			bbox->yMin = glyph_bbox.yMin;
		if (bbox->xMax < glyph_bbox.xMax)
			bbox->xMax = glyph_bbox.xMax;
		if (bbox->yMax < glyph_bbox.yMax)
			bbox->yMax = glyph_bbox.yMax;

		x += pos->x_advance;
		y -= pos->y_advance;

		info++;
		pos++;
   	}

	if (bbox->xMin > bbox->xMax) {
		bbox->xMin = 0;
		bbox->xMax = 0;
		bbox->yMin = 0;
		bbox->yMax = 0;
	} else {
		bbox->xMin >>= 6;
		bbox->xMax >>= 6;
		bbox->yMin >>= 6;
		bbox->yMax >>= 6;
	}
}

static void shape(const jchar *text, int start, int len, hb_font_t *font, hb_buffer_t *buf)
{
	hb_buffer_clear_contents(buf);
	hb_buffer_set_direction(buf, HB_DIRECTION_RTL);
	hb_buffer_set_script(buf, HB_SCRIPT_ARABIC);
	hb_buffer_set_language(buf, hb_language_from_string("ar", 2));
	hb_buffer_add_utf16(buf, text, len, start, len);
	hb_shape(font, buf, NULL, 0);
}

JNIEXPORT void JNICALL Java_com_grafian_quran_text_NativeRenderer_loadFont
  (JNIEnv *env, jclass cls, jbyteArray blob)
{
	// Initialize FreeType library for the first time
	if (!ft_ready) {
		if (FT_Init_FreeType(&ft_library)) {
			__android_log_print(6, LOG_TAG, "Error initializing FreeType");
			return;
		}
		ft_ready = 1;
	}

	// Release previous font
	if (font_blob) {
		FT_Done_Face(ft_face);
		free(font_blob);
		font_blob = 0;
	}

	// Clone blob
	int size = (*env)->GetArrayLength(env, blob);
	jbyte *bytes = (*env)->GetByteArrayElements(env, blob, 0);
	font_blob = (jbyte *) malloc(size);
	memcpy(font_blob, bytes, size);
	(*env)->ReleaseByteArrayElements(env, blob, bytes, JNI_ABORT);

	// Actual font loading
	if (FT_New_Memory_Face(ft_library, font_blob, size, 0, &ft_face)) {
		__android_log_print(6, LOG_TAG, "Cannot load font");
		free(font_blob);
		font_blob = 0;
		return;
	}
}

JNIEXPORT jintArray JNICALL Java_com_grafian_quran_text_NativeRenderer_getTextExtent
  (JNIEnv *env, jclass cls, jstring text, jint fontSize)
{
	const jchar *ctext;
	int len;
	int w = 0;
	int h = 0;
	jint array[6];
	jintArray result;
	jboolean iscopy;
	hb_buffer_t *buf;
	hb_font_t *font;
	FT_BBox bbox;

	FT_Set_Pixel_Sizes(ft_face, 0, fontSize);
	font = hb_ft_font_create(ft_face, NULL);

	buf = hb_buffer_create();
	hb_buffer_set_unicode_funcs(buf, hb_ucdn_get_unicode_funcs());

	ctext = (*env)->GetStringChars(env, text, &iscopy);
	len = (*env)->GetStringLength(env, text);

	shape(ctext, 0, len, font, buf);
	compute_bbox(buf, &bbox);
	w = bbox.xMax - bbox.xMin + 1;
	h = bbox.yMax - bbox.yMin + 1;

	(*env)->ReleaseStringChars(env, text, ctext);
	hb_buffer_destroy(buf);
	hb_font_destroy(font);

	array[0] = w;
	array[1] = h;
	array[2] = bbox.yMax;
	array[3] = ft_face->size->metrics.height >> 6;
	array[4] = ft_face->size->metrics.ascender >> 6;
	array[5] = ft_face->size->metrics.descender >> 6;
	result = (*env)->NewIntArray(env, 6);
	(*env)->SetIntArrayRegion(env, result, 0, 6, array);
	return result;
}

JNIEXPORT void JNICALL Java_com_grafian_quran_text_NativeRenderer_renderText
  (JNIEnv *env, jclass cls, jstring text, jint fontSize, jobject bitmap)
{
	AndroidBitmapInfo abi;
	void *pixels;
	const jchar *ctext;
	int len, y;
	jboolean iscopy;
	hb_buffer_t *buf;
	hb_font_t *font;
	FT_BBox bbox;

	FT_Set_Pixel_Sizes(ft_face, 0, fontSize);
	font = hb_ft_font_create(ft_face, NULL);

	AndroidBitmap_getInfo(env, bitmap, &abi);
	AndroidBitmap_lockPixels(env, bitmap, &pixels);

	memset(pixels, 0 , abi.height * abi.stride);

	buf = hb_buffer_create();
	hb_buffer_set_unicode_funcs(buf, hb_ucdn_get_unicode_funcs());

	ctext = (*env)->GetStringChars(env, text, &iscopy);
	len = (*env)->GetStringLength(env, text);

	shape(ctext, 0, len, font, buf);
	compute_bbox(buf, &bbox);
	render_glyphs(buf, pixels, &abi, 0, bbox.yMax);

	(*env)->ReleaseStringChars(env, text, ctext);
	hb_buffer_destroy(buf);
	hb_font_destroy(font);

	AndroidBitmap_unlockPixels(env, bitmap);
}