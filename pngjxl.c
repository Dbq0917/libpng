#include "pngpriv.h"

#include <limits.h>

#ifdef PNG_JXL_SUPPORTED
#  include <jxl/cms.h>
#  include <jxl/decode.h>
#  include <jxl/encode.h>
#  include <jxl/version.h>

#  if JPEGXL_NUMERIC_VERSION < JPEGXL_COMPUTE_NUMERIC_VERSION(0, 8, 1)
#    error "libjxl 0.8.1 or later is required"
#  endif
#endif

#if defined(PNG_SIMPLIFIED_READ_SUPPORTED) ||\
    defined(PNG_SIMPLIFIED_WRITE_SUPPORTED)

/* JPEG XL container signature. */
static const png_byte png_jxl_container_sig[12] =
{
   0x00, 0x00, 0x00, 0x0c, 'J', 'X', 'L', ' ', 0x0d, 0x0a, 0x87, 0x0a
};

/* Return 1 for a complete JPEG XL signature, 2 for a possibly truncated
 * signature, and 0 for a different format.
 */
int /* PRIVATE */
png_image_jxl_probe(const png_byte *memory, size_t size)
{
   size_t i;

   if (memory == NULL || size == 0)
      return 0;

   if (memory[0] == 0xff)
   {
      if (size == 1)
         return 2;
      return memory[1] == 0x0a ? 1 : 0;
   }

   if (memory[0] == 0x00)
   {
      i = size < sizeof png_jxl_container_sig ?
          size : sizeof png_jxl_container_sig;

      if (memcmp(memory, png_jxl_container_sig, i) != 0)
         return 0;

      return i == sizeof png_jxl_container_sig ? 1 : 2;
   }

   return 0;
}

static int
png_image_jxl_error(png_image *image, png_image_error_class error_class,
    const char *message)
{
   if (image == NULL)
      return 0;

   image->warning_or_error &=
       ~((png_uint_32)PNG_IMAGE_ERROR_CLASS_MASK);
   image->warning_or_error |=
       ((png_uint_32)error_class << PNG_IMAGE_ERROR_CLASS_SHIFT);
   png_safecat(image->message, (sizeof image->message), 0, message);
   image->warning_or_error |= PNG_IMAGE_ERROR;
   png_image_free(image);
   return 0;
}

int /* PRIVATE */
png_image_unsupported_format(png_image *image, const char *format_name)
{
   if (image == NULL)
      return 0;

   if (format_name != NULL)
   {
      png_safecat(image->message, (sizeof image->message), 0,
          "unsupported image format: ");
      png_safecat(image->message, (sizeof image->message),
          strlen(image->message), format_name);
   }

   else
      png_safecat(image->message, (sizeof image->message), 0,
          "unsupported image format");

   image->warning_or_error &=
       ~((png_uint_32)PNG_IMAGE_ERROR_CLASS_MASK);
   image->warning_or_error |=
       ((png_uint_32)PNG_IMAGE_ERROR_CLASS_UNSUPPORTED_FORMAT <<
        PNG_IMAGE_ERROR_CLASS_SHIFT);
   image->warning_or_error |= PNG_IMAGE_ERROR;
   png_image_free(image);
   return 0;
}

png_uint_32
png_image_get_error_class(const png_image *image)
{
   png_uint_32 error_class;

   if (image == NULL || (image->warning_or_error & PNG_IMAGE_ERROR) == 0)
      return PNG_IMAGE_ERROR_CLASS_NONE;

   error_class = (image->warning_or_error & PNG_IMAGE_ERROR_CLASS_MASK) >>
       PNG_IMAGE_ERROR_CLASS_SHIFT;

   if (error_class == PNG_IMAGE_ERROR_CLASS_NONE)
      error_class = PNG_IMAGE_ERROR_CLASS_GENERAL;

   return error_class;
}

#ifdef PNG_JXL_SUPPORTED

typedef struct
{
   JxlDecoder *decoder;
   JxlBasicInfo info;
   JxlColorEncoding color_encoding;
   png_byte *input;
   size_t input_size;
   png_byte *icc;
   size_t icc_size;
   unsigned int owns_input :1;
   unsigned int have_info :1;
   unsigned int have_color :1;
   unsigned int have_encoding :1;
   unsigned int decoded :1;
} png_jxl_state;

static int
png_jxl_is_sRGB(const JxlColorEncoding *color)
{
   if (color->color_space == JXL_COLOR_SPACE_GRAY)
      return 1;

   if (color->color_space != JXL_COLOR_SPACE_RGB)
      return 0;

   if (color->white_point != JXL_WHITE_POINT_D65 ||
       color->primaries != JXL_PRIMARIES_SRGB)
      return 0;

   return 1;
}

static int
png_jxl_profile_matches_format(const png_jxl_state *state,
    png_uint_32 format)
{
   int is_linear = (format & PNG_FORMAT_FLAG_LINEAR) != 0;

   if (state->have_encoding == 0)
      return 0;

   if ((format & PNG_FORMAT_FLAG_COLOR) != 0)
   {
      if (state->color_encoding.color_space != JXL_COLOR_SPACE_RGB ||
          state->color_encoding.white_point != JXL_WHITE_POINT_D65 ||
          state->color_encoding.primaries != JXL_PRIMARIES_SRGB)
         return 0;
   }

   else if (state->color_encoding.color_space != JXL_COLOR_SPACE_GRAY)
      return 0;

   return state->color_encoding.transfer_function ==
       (is_linear != 0 ? JXL_TRANSFER_FUNCTION_LINEAR :
       JXL_TRANSFER_FUNCTION_SRGB);
}

static int
png_jxl_get_icc(png_jxl_state *state)
{
   JxlDecoderStatus status;
   size_t size = 0;
   JxlColorProfileTarget target = JXL_COLOR_PROFILE_TARGET_ORIGINAL;

   status = JxlDecoderGetICCProfileSize(state->decoder, target, &size);

   if (status != JXL_DEC_SUCCESS || size == 0)
   {
      target = JXL_COLOR_PROFILE_TARGET_DATA;
      status = JxlDecoderGetICCProfileSize(state->decoder, target, &size);
   }

   if (status == JXL_DEC_SUCCESS && size > 0)
   {
      png_byte *icc = png_voidcast(png_byte *, malloc(size));

      if (icc == NULL)
         return 0;

      status = JxlDecoderGetColorAsICCProfile(state->decoder, target, icc,
          size);

      if (status == JXL_DEC_SUCCESS)
      {
         state->icc = icc;
         state->icc_size = size;
      }

      else
         free(icc);
   }

   return 1;
}

static void
png_jxl_free_decoder(png_jxl_state *state)
{
   if (state->decoder != NULL)
   {
      JxlDecoderDestroy(state->decoder);
      state->decoder = NULL;
   }

   if (state->input != NULL && state->owns_input)
      free(state->input);

   state->input = NULL;
   state->input_size = 0;
   state->owns_input = 0;
}

void /* PRIVATE */
png_image_jxl_free(void *jxl)
{
   png_jxl_state *state = png_voidcast(png_jxl_state *, jxl);

   if (state != NULL)
   {
      png_jxl_free_decoder(state);
      free(state->icc);
      free(state);
   }
}

static int
png_jxl_begin(png_image *image, const png_byte *memory, size_t size,
    int copy_input)
{
   png_jxl_state *state;
   JxlDecoderStatus status;
   size_t input_size;
   png_byte *input;
   int have_basic = 0;
   int have_color = 0;

   if (image == NULL || image->opaque == NULL || memory == NULL || size == 0)
      return png_image_jxl_error(image,
          PNG_IMAGE_ERROR_CLASS_CORRUPT_DATA,
          "JPEG XL: empty or invalid input");

   if (image->opaque->jxl != NULL)
      return png_image_jxl_error(image,
          PNG_IMAGE_ERROR_CLASS_GENERAL, "JPEG XL: decoder already active");

   state = png_voidcast(png_jxl_state *, calloc(1, sizeof *state));

   if (state == NULL)
      return png_image_jxl_error(image, PNG_IMAGE_ERROR_CLASS_GENERAL,
          "JPEG XL: out of memory");

   image->opaque->jxl = state;
   input_size = size;
   input = png_constcast(png_byte *, memory);

   if (copy_input != 0)
   {
      input = png_voidcast(png_byte *, malloc(size));

      if (input == NULL)
         return png_image_jxl_error(image, PNG_IMAGE_ERROR_CLASS_GENERAL,
             "JPEG XL: out of memory");

      memcpy(input, memory, size);
      state->owns_input = 1;
   }

   state->input = input;
   state->input_size = input_size;
   state->decoder = JxlDecoderCreate(NULL);

   if (state->decoder == NULL)
      return png_image_jxl_error(image, PNG_IMAGE_ERROR_CLASS_GENERAL,
          "JPEG XL: cannot create decoder");

   if (JxlDecoderSetUnpremultiplyAlpha(state->decoder, JXL_TRUE) !=
       JXL_DEC_SUCCESS)
      return png_image_jxl_error(image, PNG_IMAGE_ERROR_CLASS_CODEC,
          "JPEG XL: cannot configure alpha output");

   {
      const JxlCmsInterface *cms = JxlGetDefaultCms();

      if (cms == NULL ||
          JxlDecoderSetCms(state->decoder, *cms) != JXL_DEC_SUCCESS)
         return png_image_jxl_error(image, PNG_IMAGE_ERROR_CLASS_CODEC,
             "JPEG XL: color management is unavailable");
   }

   status = JxlDecoderSubscribeEvents(state->decoder,
       JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FRAME |
       JXL_DEC_FULL_IMAGE);

   if (status != JXL_DEC_SUCCESS)
      return png_image_jxl_error(image, PNG_IMAGE_ERROR_CLASS_CODEC,
          "JPEG XL: cannot initialize decoder");

   status = JxlDecoderSetInput(state->decoder, state->input,
       state->input_size);

   if (status != JXL_DEC_SUCCESS)
      return png_image_jxl_error(image, PNG_IMAGE_ERROR_CLASS_CODEC,
          "JPEG XL: cannot set decoder input");

   JxlDecoderCloseInput(state->decoder);

   for (;;)
   {
      status = JxlDecoderProcessInput(state->decoder);

      if (status == JXL_DEC_BASIC_INFO)
      {
         if (JxlDecoderGetBasicInfo(state->decoder, &state->info) !=
             JXL_DEC_SUCCESS)
            return png_image_jxl_error(image,
                PNG_IMAGE_ERROR_CLASS_CORRUPT_DATA,
                "JPEG XL: invalid basic information");

         state->have_info = 1;
         have_basic = 1;
      }

      else if (status == JXL_DEC_COLOR_ENCODING)
      {
         status = JxlDecoderGetColorAsEncodedProfile(state->decoder,
             JXL_COLOR_PROFILE_TARGET_ORIGINAL, &state->color_encoding);

         if (status == JXL_DEC_SUCCESS)
            state->have_encoding = 1;

         if (png_jxl_get_icc(state) == 0)
            return png_image_jxl_error(image,
                PNG_IMAGE_ERROR_CLASS_GENERAL,
                "JPEG XL: out of memory");

         state->have_color = 1;
         have_color = 1;
      }

      else if (status == JXL_DEC_NEED_MORE_INPUT ||
               status == JXL_DEC_ERROR)
         return png_image_jxl_error(image,
             PNG_IMAGE_ERROR_CLASS_CORRUPT_DATA,
             "JPEG XL: corrupt or truncated image");

      else if (status == JXL_DEC_SUCCESS)
         return png_image_jxl_error(image,
             PNG_IMAGE_ERROR_CLASS_CORRUPT_DATA,
             "JPEG XL: image ended before its header");

      else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER ||
               status == JXL_DEC_FULL_IMAGE || status == JXL_DEC_FRAME)
         return png_image_jxl_error(image,
             PNG_IMAGE_ERROR_CLASS_CORRUPT_DATA,
             "JPEG XL: image data before color information");

      if (have_basic != 0 && have_color != 0)
         break;
   }

   if (state->have_info == 0 || state->have_color == 0)
      return png_image_jxl_error(image, PNG_IMAGE_ERROR_CLASS_CORRUPT_DATA,
          "JPEG XL: incomplete image header");

   image->width = state->info.xsize;
   image->height = state->info.ysize;

   if (state->info.orientation == JXL_ORIENT_TRANSPOSE ||
       state->info.orientation == JXL_ORIENT_ROTATE_90_CW ||
       state->info.orientation == JXL_ORIENT_ANTI_TRANSPOSE ||
       state->info.orientation == JXL_ORIENT_ROTATE_90_CCW)
   {
      image->width = state->info.ysize;
      image->height = state->info.xsize;
   }

   image->format = 0;

   if (state->info.num_color_channels != 1)
      image->format |= PNG_FORMAT_FLAG_COLOR;

   if (state->info.alpha_bits != 0)
      image->format |= PNG_FORMAT_FLAG_ALPHA;

   if (state->info.bits_per_sample > 8 ||
       state->info.exponent_bits_per_sample != 0)
      image->format |= PNG_FORMAT_FLAG_LINEAR;

   image->colormap_entries = 0;
   image->flags |= PNG_IMAGE_FLAG_FORMAT_JPEGXL;

   if (state->have_encoding != 0 && png_jxl_is_sRGB(&state->color_encoding) == 0)
      image->flags |= PNG_IMAGE_FLAG_COLORSPACE_NOT_sRGB;

   return 1;
}

int /* PRIVATE */
png_image_jxl_read_memory(png_image *image, const png_byte *memory,
    size_t size)
{
   return png_jxl_begin(image, memory, size, 0);
}

#ifdef PNG_STDIO_SUPPORTED
int /* PRIVATE */
png_image_jxl_read_stdio_prefix(png_image *image, FILE *file,
    const png_byte *prefix, size_t prefix_size)
{
   png_byte *buffer = NULL;
   size_t size = 0;
   size_t capacity = prefix_size != 0 ? prefix_size : 65536;

   if (file == NULL)
      return png_image_jxl_error(image, PNG_IMAGE_ERROR_CLASS_IO,
          "JPEG XL: invalid input stream");

   buffer = png_voidcast(png_byte *, malloc(capacity));

   if (buffer == NULL)
      return png_image_jxl_error(image, PNG_IMAGE_ERROR_CLASS_GENERAL,
          "JPEG XL: out of memory");

   if (prefix_size != 0)
   {
      memcpy(buffer, prefix, prefix_size);
      size = prefix_size;
   }

   for (;;)
   {
      size_t count;

      if (size == capacity)
      {
         size_t new_capacity = capacity == 0 ? 65536 : capacity * 2;
         png_byte *new_buffer;

         if (new_capacity < capacity)
         {
            free(buffer);
            return png_image_jxl_error(image,
                PNG_IMAGE_ERROR_CLASS_GENERAL,
                "JPEG XL: input is too large");
         }

         new_buffer = png_voidcast(png_byte *, realloc(buffer, new_capacity));

         if (new_buffer == NULL)
         {
            free(buffer);
            return png_image_jxl_error(image,
                PNG_IMAGE_ERROR_CLASS_GENERAL,
                "JPEG XL: out of memory");
         }

         buffer = new_buffer;
         capacity = new_capacity;
      }

      count = fread(buffer + size, 1, capacity - size, file);
      size += count;

      if (count == 0)
      {
         if (ferror(file))
         {
            free(buffer);
            return png_image_jxl_error(image, PNG_IMAGE_ERROR_CLASS_IO,
                "JPEG XL: read error");
         }

         break;
      }
   }

   if (buffer == NULL || size == 0)
   {
      free(buffer);
      return png_image_jxl_error(image, PNG_IMAGE_ERROR_CLASS_CORRUPT_DATA,
          "JPEG XL: empty input");
   }

   {
      int result = png_jxl_begin(image, buffer, size, 1);

      free(buffer);
      return result;
   }
}

int /* PRIVATE */
png_image_jxl_read_stdio(png_image *image, FILE *file)
{
   return png_image_jxl_read_stdio_prefix(image, file, NULL, 0);
}
#endif /* STDIO */

static int
png_jxl_set_output(png_image *image, png_jxl_state *state, int channels)
{
   JxlColorEncoding target;
   JxlDecoderStatus status;
   int is_gray = channels == 1 || channels == 2;
   int is_linear =
       (image->format & PNG_FORMAT_FLAG_LINEAR) != 0 ? 1 : 0;

   if (is_linear != 0)
      JxlColorEncodingSetToLinearSRGB(&target, is_gray);
   else
      JxlColorEncodingSetToSRGB(&target, is_gray);

   status = JxlDecoderSetOutputColorProfile(state->decoder, &target, NULL, 0);

   if (status != JXL_DEC_SUCCESS)
      return 0;

   return 1;
}

static void
png_jxl_copy_row(png_image *image, png_jxl_state *state, const png_byte *src,
    png_byte *dst, int src_channels, int dst_channels, int component_size,
    const png_color *background)
{
   png_uint_32 width = image->width;
   png_uint_32 x;
   int dst_color = (image->format & PNG_FORMAT_FLAG_COLOR) != 0;
   int dst_alpha = (image->format & PNG_FORMAT_FLAG_ALPHA) != 0;
   int associate = (image->format & PNG_FORMAT_FLAG_ASSOCIATED_ALPHA) != 0;
   int src_alpha = state->info.alpha_bits != 0;
   int afirst = 0;
   int bgr = 0;
   int dst_color_start = 0;
   int dst_alpha_index = -1;
   int r_index = 0;
   int g_index = 1;
   int b_index = 2;

#ifdef PNG_FORMAT_AFIRST_SUPPORTED
   afirst = (image->format & PNG_FORMAT_FLAG_AFIRST) != 0;
#endif
#ifdef PNG_FORMAT_BGR_SUPPORTED
   bgr = (image->format & PNG_FORMAT_FLAG_BGR) != 0;
#endif

   if (afirst != 0)
   {
      dst_alpha_index = 0;
      dst_color_start = dst_alpha != 0 ? 1 : 0;
   }

   else if (dst_alpha != 0)
      dst_alpha_index = dst_channels - 1;

   if (bgr != 0 && dst_color != 0)
   {
      r_index = 2;
      b_index = 0;
   }

   for (x = 0; x < width; ++x)
   {
      const png_byte *sp = src + x * src_channels * component_size;
      png_byte *dp = dst + x * dst_channels * component_size;
      int source_has_alpha = src_alpha != 0;

      if (component_size == 1)
      {
         unsigned int alpha = 255;
         unsigned int values[4];
         int i;

         if (src_channels == (dst_color != 0 ? 4 : 2))
            alpha = sp[src_channels - 1];

         values[0] = sp[0];

         if (src_channels > 1)
            values[1] = sp[1];

         if (src_channels > 2)
            values[2] = sp[2];

         if (src_channels > 3)
            values[3] = sp[3];

         if (dst_color == 0)
         {
            if (dst_alpha != 0)
            {
               unsigned int value = values[0];

               if (associate != 0)
                  value = (value * alpha + 127) / 255;

               dp[dst_color_start] = (png_byte)value;
            }

            else if (source_has_alpha != 0)
            {
               unsigned int bg = background != NULL ? background->green : 0;
               dp[0] = (png_byte)/*SAFE*/(
                   (values[0] * alpha + bg * (255 - alpha) + 127) / 255);
            }

            else
               dp[0] = (png_byte)values[0];
         }

         else
         {
            unsigned int color[3];

            color[0] = values[0];
            color[1] = values[1];
            color[2] = values[2];

            if (dst_alpha == 0 && source_has_alpha != 0)
            {
               unsigned int bg[3];

               bg[0] = background != NULL ? background->red : 0;
               bg[1] = background != NULL ? background->green : 0;
               bg[2] = background != NULL ? background->blue : 0;

               for (i = 0; i < 3; ++i)
                  color[i] = (color[i] * alpha + bg[i] * (255 - alpha) + 127)
                      / 255;
            }

            else if (dst_alpha != 0 && associate != 0)
            {
               for (i = 0; i < 3; ++i)
                  color[i] = (color[i] * alpha + 127) / 255;
            }

            dp[dst_color_start + r_index] = (png_byte)color[0];
            dp[dst_color_start + g_index] = (png_byte)color[1];
            dp[dst_color_start + b_index] = (png_byte)color[2];
         }

         if (dst_alpha != 0)
            dp[dst_alpha_index] = (png_byte)alpha;
      }

      else
      {
         const png_uint_16 *sp16 = png_constcast(const png_uint_16 *, sp);
         png_uint_16 *dp16 = png_aligncast(png_uint_16 *, dp);
         unsigned int alpha = 65535;
         unsigned int values[4];
         int i;

         if (src_channels == (dst_color != 0 ? 4 : 2))
            alpha = sp16[src_channels - 1];

         values[0] = sp16[0];

         if (src_channels > 1)
            values[1] = sp16[1];

         if (src_channels > 2)
            values[2] = sp16[2];

         if (src_channels > 3)
            values[3] = sp16[3];

         if (dst_color == 0)
         {
            if (dst_alpha != 0)
            {
               unsigned int value = values[0];

               if (associate != 0)
                  value = (unsigned int)/*SAFE*/(
                      ((png_uint_32)value * alpha + 32767) / 65535);

               dp16[dst_color_start] = (png_uint_16)value;
            }

            else if (source_has_alpha != 0)
               dp16[0] = (png_uint_16)/*SAFE*/(
                   ((png_uint_32)values[0] * alpha + 32767) / 65535);

            else
               dp16[0] = (png_uint_16)values[0];
         }

         else
         {
            int index[3];

            index[0] = r_index;
            index[1] = g_index;
            index[2] = b_index;

            for (i = 0; i < 3; ++i)
            {
               unsigned int value = values[i];

               if (dst_alpha == 0 && source_has_alpha != 0)
                  value = (unsigned int)/*SAFE*/(
                      ((png_uint_32)value * alpha + 32767) / 65535);

               else if (dst_alpha != 0 && associate != 0)
                  value = (unsigned int)/*SAFE*/(
                      ((png_uint_32)value * alpha + 32767) / 65535);

               dp16[dst_color_start + index[i]] = (png_uint_16)value;
            }
         }

         if (dst_alpha != 0)
            dp16[dst_alpha_index] = (png_uint_16)alpha;
      }
   }
}

int /* PRIVATE */
png_image_jxl_finish_read(png_image *image, const png_color *background,
    void *buffer, png_int_32 row_stride)
{
   png_jxl_state *state;
   JxlPixelFormat format;
   JxlDecoderStatus status;
   png_byte *decoded = NULL;
   size_t decoded_size = 0;
   int dst_channels;
   int component_size;
   int want_alpha;
   int temp_channels;
   int have_buffer = 0;
   int complete = 0;

   if (image == NULL || image->opaque == NULL ||
       image->opaque->jxl == NULL || buffer == NULL ||
       ((png_jxl_state *)image->opaque->jxl)->decoder == NULL)
      return png_image_jxl_error(image, PNG_IMAGE_ERROR_CLASS_GENERAL,
          "JPEG XL: invalid read state");

   if ((image->format & PNG_FORMAT_FLAG_COLORMAP) != 0)
      return png_image_jxl_error(image,
          PNG_IMAGE_ERROR_CLASS_UNSUPPORTED_FORMAT,
          "JPEG XL: color-map output is not supported");

   state = png_voidcast(png_jxl_state *, image->opaque->jxl);
   dst_channels = (int)PNG_IMAGE_PIXEL_CHANNELS(image->format);
   component_size = (int)PNG_IMAGE_PIXEL_COMPONENT_SIZE(image->format);
   want_alpha = (image->format & PNG_FORMAT_FLAG_ALPHA) != 0;
   temp_channels = dst_channels;

   if (want_alpha == 0 && state->info.alpha_bits != 0)
      temp_channels += 1;

   if (png_jxl_set_output(image, state, temp_channels) == 0)
      return png_image_jxl_error(image, PNG_IMAGE_ERROR_CLASS_CODEC,
          "JPEG XL: cannot configure output color space");

   format.num_channels = (png_uint_32)temp_channels;
   format.data_type = component_size == 2 ? JXL_TYPE_UINT16 : JXL_TYPE_UINT8;
   format.endianness = JXL_NATIVE_ENDIAN;
   format.align = 1;

   status = JxlDecoderImageOutBufferSize(state->decoder, &format,
       &decoded_size);

   if (status != JXL_DEC_SUCCESS)
      return png_image_jxl_error(image, PNG_IMAGE_ERROR_CLASS_CODEC,
          "JPEG XL: cannot determine output size");

   decoded = png_voidcast(png_byte *, malloc(decoded_size));

   if (decoded == NULL)
      return png_image_jxl_error(image, PNG_IMAGE_ERROR_CLASS_GENERAL,
          "JPEG XL: out of memory");

   for (;;)
   {
      status = JxlDecoderProcessInput(state->decoder);

      if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER)
      {
         if (have_buffer != 0)
            break;

         if (JxlDecoderSetImageOutBuffer(state->decoder, &format, decoded,
             decoded_size) != JXL_DEC_SUCCESS)
            break;

         have_buffer = 1;
      }

      else if (status == JXL_DEC_FULL_IMAGE)
      {
         complete = 1;
         break;
      }

      else if (status == JXL_DEC_FRAME)
      {
         /* Continue to the output-buffer request for the first frame. */
      }

      else if (status == JXL_DEC_SUCCESS)
         break;

      else
         break;
   }

   if (complete == 0 || have_buffer == 0)
   {
      free(decoded);
      return png_image_jxl_error(image,
          PNG_IMAGE_ERROR_CLASS_CORRUPT_DATA,
          "JPEG XL: corrupt or truncated image data");
   }

   {
      ptrdiff_t stride = row_stride;
      ptrdiff_t row_bytes;
      png_byte *first;
      png_uint_32 y;

      if (stride == 0)
         stride = (ptrdiff_t)image->width * dst_channels;

      row_bytes = (stride < 0 ? -stride : stride) * component_size;
      first = png_voidcast(png_byte *, buffer);

      if (stride < 0)
         first += (ptrdiff_t)(image->height - 1) * row_bytes;

      for (y = 0; y < image->height; ++y)
      {
         const png_byte *src = decoded +
             (size_t)y * image->width * temp_channels * component_size;
         png_byte *dst = first + (stride < 0 ? -(ptrdiff_t)y : (ptrdiff_t)y) *
             row_bytes;

         png_jxl_copy_row(image, state, src, dst, temp_channels, dst_channels,
             component_size, background);
      }
   }

   free(decoded);
   state->decoded = 1;
   png_image_jxl_finish(image);
   return 1;
}

void /* PRIVATE */
png_image_jxl_finish(png_image *image)
{
   if (image != NULL && image->opaque != NULL &&
       image->opaque->jxl != NULL)
   {
      png_jxl_state *state =
          png_voidcast(png_jxl_state *, image->opaque->jxl);

      png_jxl_free_decoder(state);
      state->decoded = 1;
   }
}

int
png_image_get_icc_profile(png_image *image, void *profile,
    size_t *profile_size)
{
   png_jxl_state *state;

   if (image == NULL || image->version != PNG_IMAGE_VERSION ||
       profile_size == NULL)
      return 0;

   if (image->opaque == NULL || image->opaque->jxl == NULL)
   {
      *profile_size = 0;
      return 1;
   }

   state = png_voidcast(png_jxl_state *, image->opaque->jxl);

   if (profile == NULL)
   {
      *profile_size = state->icc_size;
      return 1;
   }

   if (*profile_size < state->icc_size)
   {
      *profile_size = state->icc_size;
      png_safecat(image->message, (sizeof image->message), 0,
          "png_image_get_icc_profile: buffer too small");
      return 0;
   }

   if (state->icc_size > 0)
      memcpy(profile, state->icc, state->icc_size);

   *profile_size = state->icc_size;
   return 1;
}

#else /* !PNG_JXL_SUPPORTED */

int /* PRIVATE */
png_image_jxl_read_memory(png_image *image, const png_byte *memory,
    size_t size)
{
   PNG_UNUSED(memory)
   PNG_UNUSED(size)
   return png_image_jxl_error(image,
       PNG_IMAGE_ERROR_CLASS_UNSUPPORTED_FORMAT,
       "JPEG XL support is not enabled in this build");
}

#ifdef PNG_STDIO_SUPPORTED
int /* PRIVATE */
png_image_jxl_read_stdio(png_image *image, FILE *file)
{
   PNG_UNUSED(file)
   return png_image_jxl_error(image,
       PNG_IMAGE_ERROR_CLASS_UNSUPPORTED_FORMAT,
       "JPEG XL support is not enabled in this build");
}

int /* PRIVATE */
png_image_jxl_read_stdio_prefix(png_image *image, FILE *file,
    const png_byte *prefix, size_t prefix_size)
{
   PNG_UNUSED(file)
   PNG_UNUSED(prefix)
   PNG_UNUSED(prefix_size)
   return png_image_jxl_read_stdio(image, NULL);
}
#endif

int /* PRIVATE */
png_image_jxl_finish_read(png_image *image, const png_color *background,
    void *buffer, png_int_32 row_stride)
{
   PNG_UNUSED(background)
   PNG_UNUSED(buffer)
   PNG_UNUSED(row_stride)
   return png_image_jxl_error(image,
       PNG_IMAGE_ERROR_CLASS_UNSUPPORTED_FORMAT,
       "JPEG XL support is not enabled in this build");
}

void /* PRIVATE */
png_image_jxl_finish(png_image *image)
{
   PNG_UNUSED(image)
}

void /* PRIVATE */
png_image_jxl_free(void *jxl)
{
   PNG_UNUSED(jxl)
}

int
png_image_get_icc_profile(png_image *image, void *profile,
    size_t *profile_size)
{
   PNG_UNUSED(image)
   PNG_UNUSED(profile)

   if (profile_size != NULL)
      *profile_size = 0;

   return 0;
}

#endif /* PNG_JXL_SUPPORTED */

#ifdef PNG_JXL_SUPPORTED

static int
png_jxl_write_error(png_image *image, png_image_error_class error_class,
    const char *message)
{
   if (image == NULL)
      return 0;

   image->warning_or_error &=
       ~((png_uint_32)PNG_IMAGE_ERROR_CLASS_MASK);
   image->warning_or_error |=
       ((png_uint_32)error_class << PNG_IMAGE_ERROR_CLASS_SHIFT);
   png_safecat(image->message, (sizeof image->message), 0, message);
   image->warning_or_error |= PNG_IMAGE_ERROR;
   return 0;
}

static int
png_jxl_write_pixels(png_image *image, const void *buffer,
    png_int_32 row_stride, png_byte **result, size_t *result_size)
{
   int channels;
   int component_size;
   size_t row_bytes;
   size_t total;
   ptrdiff_t stride;
   ptrdiff_t input_row_bytes;
   png_byte *pixels;
   const png_byte *first;
   png_uint_32 y;
   png_uint_32 x;
   int afirst = 0;
   int bgr = 0;
   int color;
   int alpha;
   int color_start = 0;
   int alpha_index = -1;
   int r_index = 0;
   int g_index = 1;
   int b_index = 2;

   if (buffer == NULL || image->width == 0 || image->height == 0)
      return png_jxl_write_error(image, PNG_IMAGE_ERROR_CLASS_GENERAL,
          "JPEG XL write: invalid argument");

   if ((image->format & PNG_FORMAT_FLAG_COLORMAP) != 0)
      return png_jxl_write_error(image,
          PNG_IMAGE_ERROR_CLASS_UNSUPPORTED_FORMAT,
          "JPEG XL write: color-map input is not supported");

   if ((image->format & ~(PNG_FORMAT_FLAG_ALPHA | PNG_FORMAT_FLAG_COLOR |
       PNG_FORMAT_FLAG_LINEAR
#ifdef PNG_FORMAT_BGR_SUPPORTED
       | PNG_FORMAT_FLAG_BGR
#endif
#ifdef PNG_FORMAT_AFIRST_SUPPORTED
       | PNG_FORMAT_FLAG_AFIRST
#endif
       | PNG_FORMAT_FLAG_ASSOCIATED_ALPHA)) != 0)
      return png_jxl_write_error(image, PNG_IMAGE_ERROR_CLASS_GENERAL,
          "JPEG XL write: unsupported image format");

   channels = (int)PNG_IMAGE_PIXEL_CHANNELS(image->format);
   component_size = (int)PNG_IMAGE_PIXEL_COMPONENT_SIZE(image->format);
   color = (image->format & PNG_FORMAT_FLAG_COLOR) != 0;
   alpha = (image->format & PNG_FORMAT_FLAG_ALPHA) != 0;

#ifdef PNG_FORMAT_AFIRST_SUPPORTED
   afirst = (image->format & PNG_FORMAT_FLAG_AFIRST) != 0;
#endif
#ifdef PNG_FORMAT_BGR_SUPPORTED
   bgr = (image->format & PNG_FORMAT_FLAG_BGR) != 0;
#endif

   if (afirst != 0)
   {
      alpha_index = 0;
      color_start = alpha != 0 ? 1 : 0;
   }

   else if (alpha != 0)
      alpha_index = channels - 1;

   if (bgr != 0 && color != 0)
   {
      r_index = 2;
      b_index = 0;
   }

   if (image->width > SIZE_MAX / (size_t)channels / (size_t)component_size)
      return png_jxl_write_error(image, PNG_IMAGE_ERROR_CLASS_GENERAL,
          "JPEG XL write: image is too large");

   row_bytes = (size_t)image->width * channels * component_size;

   if (image->height > SIZE_MAX / row_bytes)
      return png_jxl_write_error(image, PNG_IMAGE_ERROR_CLASS_GENERAL,
          "JPEG XL write: image is too large");

   total = row_bytes * image->height;
   pixels = png_voidcast(png_byte *, malloc(total));

   if (pixels == NULL)
      return png_jxl_write_error(image, PNG_IMAGE_ERROR_CLASS_GENERAL,
          "JPEG XL write: out of memory");

   stride = row_stride;

   if (stride == 0)
      stride = (ptrdiff_t)image->width * channels;

   if ((stride < 0 ? -stride : stride) <
       (ptrdiff_t)image->width * channels)
   {
      free(pixels);
      return png_jxl_write_error(image, PNG_IMAGE_ERROR_CLASS_GENERAL,
          "JPEG XL write: row stride is too small");
   }

   input_row_bytes = (stride < 0 ? -stride : stride) * component_size;
   first = png_voidcast(const png_byte *, buffer);

   if (stride < 0)
      first += (ptrdiff_t)(image->height - 1) * input_row_bytes;

   for (y = 0; y < image->height; ++y)
   {
      const png_byte *src = first +
          (stride < 0 ? -(ptrdiff_t)y : (ptrdiff_t)y) * input_row_bytes;
      png_byte *dst = pixels + (size_t)y * row_bytes;

      for (x = 0; x < image->width; ++x)
      {
         const png_byte *sp = src + x * channels * component_size;
         png_byte *dp = dst + x * channels * component_size;
         if (color != 0)
         {
            if (component_size == 1)
            {
               dp[0] = sp[color_start + r_index];
               dp[1] = sp[color_start + g_index];
               dp[2] = sp[color_start + b_index];
            }

            else
            {
               const png_uint_16 *sp16 =
                   png_constcast(const png_uint_16 *, sp);
               png_uint_16 *dp16 = png_aligncast(png_uint_16 *, dp);

               dp16[0] = sp16[color_start + r_index];
               dp16[1] = sp16[color_start + g_index];
               dp16[2] = sp16[color_start + b_index];
            }
         }

         else if (component_size == 1)
            dp[0] = sp[color_start];

         else
         {
            const png_uint_16 *sp16 =
                png_constcast(const png_uint_16 *, sp);
            png_uint_16 *dp16 = png_aligncast(png_uint_16 *, dp);

            dp16[0] = sp16[color_start];
         }

         if (alpha != 0)
         {
            if (component_size == 1)
               dp[channels - 1] = sp[alpha_index];
            else
            {
               const png_uint_16 *sp16 =
                   png_constcast(const png_uint_16 *, sp);
               png_uint_16 *dp16 = png_aligncast(png_uint_16 *, dp);

               dp16[channels - 1] = sp16[alpha_index];
            }
         }

      }
   }

   *result = pixels;
   *result_size = total;
   return 1;
}

int
png_image_write_jxl_to_memory(png_image *image, void *memory,
    png_alloc_size_t *memory_bytes, const void *buffer, png_int_32 row_stride,
    const void *colormap, const png_image_jxl_options *options)
{
   png_byte *pixels = NULL;
   size_t pixels_size = 0;
   png_byte *output = NULL;
   size_t output_size = 0;
   size_t output_capacity = 0;
   JxlEncoder *encoder = NULL;
   JxlEncoderFrameSettings *frame = NULL;
   JxlBasicInfo info;
   JxlPixelFormat pixel_format;
   JxlColorEncoding color_encoding;
   JxlEncoderStatus status;
   int lossless = 1;
   int quality = 90;
   int is_linear;
   const png_byte *icc_profile = NULL;
   size_t icc_profile_size = 0;

   PNG_UNUSED(colormap)

   if (image == NULL || image->version != PNG_IMAGE_VERSION ||
       memory_bytes == NULL)
      return 0;

   if (memory == NULL)
      *memory_bytes = 0;

   if (options != NULL)
   {
      if (options->version != PNG_IMAGE_JXL_OPTIONS_VERSION)
      {
         int result = png_jxl_write_error(image,
             PNG_IMAGE_ERROR_CLASS_GENERAL,
             "JPEG XL write: incorrect options version");

         *memory_bytes = 0;
         png_image_free(image);
         return result;
      }

      lossless = options->lossless != 0 || options->quality >= 100;
      quality = options->quality;

      if (quality < 0)
         quality = 0;
      else if (quality > 99)
         quality = 99;
   }

   if (options != NULL && options->icc_profile != NULL &&
       options->icc_profile_size > 0)
   {
      icc_profile = options->icc_profile;
      icc_profile_size = options->icc_profile_size;
   }

   else if (image->opaque != NULL && image->opaque->jxl != NULL)
   {
      png_jxl_state *state =
          png_voidcast(png_jxl_state *, image->opaque->jxl);

      if (state->icc_size > 0 &&
          png_jxl_profile_matches_format(state, image->format) != 0)
      {
         icc_profile = state->icc;
         icc_profile_size = state->icc_size;
      }
   }

   if (png_jxl_write_pixels(image, buffer, row_stride, &pixels,
       &pixels_size) == 0)
   {
      *memory_bytes = 0;
      png_image_free(image);
      return 0;
   }

   is_linear = (image->format & PNG_FORMAT_FLAG_LINEAR) != 0 ? 1 : 0;
   encoder = JxlEncoderCreate(NULL);

   if (encoder == NULL)
   {
      free(pixels);
      *memory_bytes = 0;
      (void)png_jxl_write_error(image, PNG_IMAGE_ERROR_CLASS_GENERAL,
          "JPEG XL write: cannot create encoder");
      png_image_free(image);
      return 0;
   }

   if (JxlEncoderUseContainer(encoder, JXL_FALSE) != JXL_ENC_SUCCESS)
      goto encode_error;

   JxlEncoderInitBasicInfo(&info);
   info.xsize = image->width;
   info.ysize = image->height;
   info.bits_per_sample = is_linear != 0 ? 16 : 8;
   info.num_color_channels =
       (image->format & PNG_FORMAT_FLAG_COLOR) != 0 ? 3 : 1;
   info.uses_original_profile = JXL_TRUE;

   if ((image->format & PNG_FORMAT_FLAG_ALPHA) != 0)
   {
      info.num_extra_channels = 1;
      info.alpha_bits = info.bits_per_sample;
      info.alpha_premultiplied =
          (image->format & PNG_FORMAT_FLAG_ASSOCIATED_ALPHA) != 0;
   }

   if (JxlEncoderSetBasicInfo(encoder, &info) != JXL_ENC_SUCCESS)
      goto encode_error;

   if (icc_profile != NULL && icc_profile_size > 0)
   {
      if (JxlEncoderSetICCProfile(encoder, icc_profile, icc_profile_size) !=
          JXL_ENC_SUCCESS)
         goto encode_error;
   }

   else
   {
      int is_gray = info.num_color_channels == 1;

      if (is_linear != 0)
         JxlColorEncodingSetToLinearSRGB(&color_encoding, is_gray);
      else
         JxlColorEncodingSetToSRGB(&color_encoding, is_gray);

      if (JxlEncoderSetColorEncoding(encoder, &color_encoding) !=
          JXL_ENC_SUCCESS)
         goto encode_error;
   }

   frame = JxlEncoderFrameSettingsCreate(encoder, NULL);

   if (frame == NULL)
      goto encode_error;

   if (JxlEncoderSetFrameLossless(frame, lossless != 0 ? JXL_TRUE :
       JXL_FALSE) != JXL_ENC_SUCCESS)
      goto encode_error;

   if (lossless == 0 &&
       JxlEncoderSetFrameDistance(frame,
           JxlEncoderDistanceFromQuality((float)quality)) != JXL_ENC_SUCCESS)
      goto encode_error;

   pixel_format.num_channels = (png_uint_32)(
       info.num_color_channels + info.num_extra_channels);
   pixel_format.data_type = is_linear != 0 ? JXL_TYPE_UINT16 : JXL_TYPE_UINT8;
   pixel_format.endianness = JXL_NATIVE_ENDIAN;
   pixel_format.align = 1;

   if (JxlEncoderAddImageFrame(frame, &pixel_format, pixels, pixels_size) !=
       JXL_ENC_SUCCESS)
      goto encode_error;

   JxlEncoderCloseInput(encoder);

   for (;;)
   {
      size_t available = output_capacity - output_size;

      if (available < 65536)
      {
         size_t new_capacity = output_capacity == 0 ? 65536 :
             output_capacity * 2;
         png_byte *new_output;

         if (new_capacity < output_capacity)
            goto encode_error;

         new_output = png_voidcast(png_byte *, realloc(output, new_capacity));

         if (new_output == NULL)
            goto encode_error;

         output = new_output;
         output_capacity = new_capacity;
         available = output_capacity - output_size;
      }

      {
         png_byte *next = output + output_size;

         status = JxlEncoderProcessOutput(encoder, &next, &available);
         output_size = (size_t)(next - output);
      }

      if (status == JXL_ENC_SUCCESS)
         break;

      if (status != JXL_ENC_NEED_MORE_OUTPUT)
         goto encode_error;
   }

   JxlEncoderDestroy(encoder);
   free(pixels);

   if (memory != NULL)
   {
      if ((size_t)*memory_bytes < output_size)
      {
         *memory_bytes = (png_alloc_size_t)output_size;
         free(output);
         png_image_free(image);
         return 0;
      }

      memcpy(memory, output, output_size);
   }

   *memory_bytes = (png_alloc_size_t)output_size;
   free(output);

   if (memory != NULL)
      png_image_free(image);

   return 1;

encode_error:
   if (encoder != NULL)
      JxlEncoderDestroy(encoder);
   free(pixels);
   free(output);
   *memory_bytes = 0;
   (void)png_jxl_write_error(image, PNG_IMAGE_ERROR_CLASS_CODEC,
       "JPEG XL write: encoding failed");
   png_image_free(image);
   return 0;
}

#ifdef PNG_SIMPLIFIED_WRITE_STDIO_SUPPORTED
int
png_image_write_jxl_to_stdio(png_image *image, FILE *file, const void *buffer,
    png_int_32 row_stride, const void *colormap,
    const png_image_jxl_options *options)
{
   void *memory = NULL;
   png_alloc_size_t size = 0;
   png_alloc_size_t actual_size = 0;
   int result;

   if (image == NULL)
      return 0;

   if (file == NULL || buffer == NULL)
   {
      (void)png_jxl_write_error(image, PNG_IMAGE_ERROR_CLASS_GENERAL,
          "JPEG XL write: invalid argument");
      png_image_free(image);
      return 0;
   }

   result = png_image_write_jxl_to_memory(image, NULL, &size, buffer,
       row_stride, colormap, options);

   if (result == 0)
      return 0;

   memory = malloc((size_t)size);

   if (memory == NULL)
   {
      (void)png_jxl_write_error(image, PNG_IMAGE_ERROR_CLASS_GENERAL,
          "JPEG XL write: out of memory");
      png_image_free(image);
      return 0;
   }

   {
      png_alloc_size_t capacity = size;

      result = png_image_write_jxl_to_memory(image, memory, &capacity, buffer,
          row_stride, colormap, options);
      actual_size = capacity;
   }

   if (result == 0 ||
       fwrite(memory, 1, (size_t)actual_size, file) != (size_t)actual_size)
   {
      free(memory);
      return png_jxl_write_error(image, PNG_IMAGE_ERROR_CLASS_IO,
          "JPEG XL write: output error");
   }

   free(memory);
   return 1;
}

int
png_image_write_jxl_to_file(png_image *image, const char *file_name,
    const void *buffer, png_int_32 row_stride, const void *colormap,
    const png_image_jxl_options *options)
{
   FILE *file;

   if (image == NULL || file_name == NULL)
      return 0;

   file = fopen(file_name, "wb");

   if (file == NULL)
   {
      (void)png_jxl_write_error(image, PNG_IMAGE_ERROR_CLASS_IO,
          "JPEG XL write: cannot open output file");
      png_image_free(image);
      return 0;
   }

   if (png_image_write_jxl_to_stdio(image, file, buffer, row_stride, colormap,
       options) == 0)
   {
      (void)fclose(file);
      (void)remove(file_name);
      return 0;
   }

   if (fflush(file) != 0 || fclose(file) != 0)
   {
      (void)remove(file_name);
      return png_jxl_write_error(image, PNG_IMAGE_ERROR_CLASS_IO,
          "JPEG XL write: output error");
   }

   return 1;
}
#endif /* SIMPLIFIED_WRITE_STDIO */

#else /* !PNG_JXL_SUPPORTED */

int
png_image_write_jxl_to_memory(png_image *image, void *memory,
    png_alloc_size_t *memory_bytes, const void *buffer, png_int_32 row_stride,
    const void *colormap, const png_image_jxl_options *options)
{
   PNG_UNUSED(memory)
   PNG_UNUSED(buffer)
   PNG_UNUSED(row_stride)
   PNG_UNUSED(colormap)
   PNG_UNUSED(options)

   if (memory_bytes != NULL)
      *memory_bytes = 0;

   if (image != NULL)
   {
      png_safecat(image->message, (sizeof image->message), 0,
          "JPEG XL support is not enabled in this build");
      image->warning_or_error &=
          ~((png_uint_32)PNG_IMAGE_ERROR_CLASS_MASK);
      image->warning_or_error |=
          ((png_uint_32)PNG_IMAGE_ERROR_CLASS_UNSUPPORTED_FORMAT <<
           PNG_IMAGE_ERROR_CLASS_SHIFT);
      image->warning_or_error |= PNG_IMAGE_ERROR;
   }

   return 0;
}

#ifdef PNG_SIMPLIFIED_WRITE_STDIO_SUPPORTED
int
png_image_write_jxl_to_stdio(png_image *image, FILE *file, const void *buffer,
    png_int_32 row_stride, const void *colormap,
    const png_image_jxl_options *options)
{
   PNG_UNUSED(file)
   PNG_UNUSED(buffer)
   PNG_UNUSED(row_stride)
   PNG_UNUSED(colormap)
   PNG_UNUSED(options)

   if (image != NULL)
   {
      png_safecat(image->message, (sizeof image->message), 0,
          "JPEG XL support is not enabled in this build");
      image->warning_or_error &=
          ~((png_uint_32)PNG_IMAGE_ERROR_CLASS_MASK);
      image->warning_or_error |=
          ((png_uint_32)PNG_IMAGE_ERROR_CLASS_UNSUPPORTED_FORMAT <<
           PNG_IMAGE_ERROR_CLASS_SHIFT);
      image->warning_or_error |= PNG_IMAGE_ERROR;
   }

   return 0;
}

int
png_image_write_jxl_to_file(png_image *image, const char *file_name,
    const void *buffer, png_int_32 row_stride, const void *colormap,
    const png_image_jxl_options *options)
{
   PNG_UNUSED(file_name)

   return png_image_write_jxl_to_stdio(image, NULL, buffer, row_stride,
       colormap, options);
}
#endif /* SIMPLIFIED_WRITE_STDIO */

#endif /* PNG_JXL_SUPPORTED */

#endif /* SIMPLIFIED_READ || SIMPLIFIED_WRITE */
