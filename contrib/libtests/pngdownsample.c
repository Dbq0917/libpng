#include "png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PNG_READ_DOWNSAMPLE_SUPPORTED

typedef struct
{
   png_uint_32 width;
   png_uint_32 height;
   unsigned int channels;
   unsigned int bit_depth;
   int swapped;
   png_byte *data;
} test_image;

static int failures;
static unsigned int status_calls;
static unsigned int status_passes;

static void
test_error(png_struct *png_ptr, png_const_charp message)
{
   fprintf(stderr, "libpng error: %s\n", message);
   longjmp(png_jmpbuf(png_ptr), 1);
}

static void
test_error_quiet(png_struct *png_ptr, png_const_charp message)
{
   (void)message;
   longjmp(png_jmpbuf(png_ptr), 1);
}

static void
test_warning(png_struct *png_ptr, png_const_charp message)
{
   (void)png_ptr;
   (void)message;
}

static void
test_status(png_struct *png_ptr, png_uint_32 row, int pass)
{
   (void)png_ptr;
   (void)row;
   status_calls++;
   status_passes |= 1U << pass;
}

static void
write_png(const char *name, png_uint_32 width, png_uint_32 height,
    int color_type, int bit_depth, int interlace, int palette, int trns)
{
   FILE *file = fopen(name, "wb");
   png_struct *png_ptr;
   png_info *info_ptr;

   if (file == NULL)
   {
      perror(name);
      exit(2);
   }

   png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, test_error,
       test_warning);
   info_ptr = png_create_info_struct(png_ptr);

   if (png_ptr == NULL || info_ptr == NULL)
      exit(2);

   if (setjmp(png_jmpbuf(png_ptr)) != 0)
   {
      fprintf(stderr, "write failed: %s\n", name);
      exit(2);
   }

   png_init_io(png_ptr, file);
   png_set_IHDR(png_ptr, info_ptr, width, height, bit_depth, color_type,
       interlace, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

   if (palette != 0)
   {
      png_color colors[16];
      png_byte alpha[16];
      int i;

      for (i = 0; i < 16; ++i)
      {
         colors[i].red = (png_byte)(i * 17);
         colors[i].green = (png_byte)(255 - i * 13);
         colors[i].blue = (png_byte)(i * 7);
         alpha[i] = (png_byte)(i * 15);
      }

      png_set_PLTE(png_ptr, info_ptr, colors, 16);
      if (trns != 0)
         png_set_tRNS(png_ptr, info_ptr, alpha, 16, NULL);
   }

   png_write_info(png_ptr, info_ptr);

   {
      size_t rowbytes = png_get_rowbytes(png_ptr, info_ptr);
      png_byte *data = (png_byte *)malloc(rowbytes * height);
      png_byte **rows = (png_byte **)malloc(sizeof (png_byte *) * height);
      png_uint_32 x, y;
      unsigned int components = 1;

      if ((color_type & PNG_COLOR_MASK_COLOR) != 0)
         components = 3;
      if ((color_type & PNG_COLOR_MASK_ALPHA) != 0)
         ++components;
      if (palette != 0)
         components = 1;

      if (data == NULL || rows == NULL)
         exit(2);

      for (y = 0; y < height; ++y)
      {
         png_byte *row = data + (size_t)y * rowbytes;
         rows[y] = row;

         for (x = 0; x < width; ++x)
         {
            unsigned int c;

            for (c = 0; c < components; ++c)
            {
               if (bit_depth == 8)
               {
                  unsigned int value =
                      (x * 13 + y * 29 + (x * y) % 251 + c * 53) & 255;
                  row[(size_t)x * components + c] =
                      (png_byte)(palette != 0 ? (value & 15) : value);
               }

               else
               {
                  unsigned int value =
                      (x * 1009 + y * 3001 + x * y * 17 + c * 7919) & 65535;
                  png_byte *bp = row +
                      ((size_t)x * components + c) * 2;
                  bp[0] = (png_byte)(value >> 8);
                  bp[1] = (png_byte)(value & 255);
               }
            }
         }
      }

      png_write_image(png_ptr, rows);
      free(rows);
      free(data);
   }

   png_write_end(png_ptr, info_ptr);
   png_destroy_write_struct(&png_ptr, &info_ptr);
   fclose(file);
}

static test_image
read_png(const char *name, png_uint_32 target_width,
    png_uint_32 target_height, int use_rows, int swap)
{
   FILE *file = fopen(name, "rb");
   png_struct *png_ptr;
   png_info *info_ptr;
   test_image image;

   memset(&image, 0, sizeof image);

   if (file == NULL)
   {
      perror(name);
      return image;
   }

   png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, test_error,
       test_warning);
   info_ptr = png_create_info_struct(png_ptr);

   if (png_ptr == NULL || info_ptr == NULL)
      return image;

   if (setjmp(png_jmpbuf(png_ptr)) != 0)
   {
      png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
      fclose(file);
      memset(&image, 0, sizeof image);
      return image;
   }

   png_init_io(png_ptr, file);
   png_read_info(png_ptr, info_ptr);

   if (swap != 0)
      png_set_swap(png_ptr);

   if (png_get_color_type(png_ptr, info_ptr) == PNG_COLOR_TYPE_PALETTE ||
       png_get_bit_depth(png_ptr, info_ptr) < 8 ||
       png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS) != 0)
      png_set_expand(png_ptr);

   if (target_width != 0)
   {
      png_set_downsample(png_ptr, target_width, target_height);
      if (use_rows != 0)
         png_set_read_status_fn(png_ptr, test_status);
   }

   png_read_update_info(png_ptr, info_ptr);
   image.width = png_get_image_width(png_ptr, info_ptr);
   image.height = png_get_image_height(png_ptr, info_ptr);
   image.channels = png_get_channels(png_ptr, info_ptr);
   image.bit_depth = png_get_bit_depth(png_ptr, info_ptr);
   image.swapped = swap;

   {
      size_t rowbytes = png_get_rowbytes(png_ptr, info_ptr);
      png_byte **rows =
          (png_byte **)malloc(sizeof (png_byte *) * image.height);
      png_uint_32 y;

      if (target_width != 0 &&
          png_get_downsample_memory(png_ptr) == 0)
      {
         fprintf(stderr, "downsample memory estimate is zero\n");
         exit(2);
      }

      image.data = (png_byte *)malloc(rowbytes * image.height);
      if (rows == NULL || image.data == NULL)
         exit(2);

      for (y = 0; y < image.height; ++y)
         rows[y] = image.data + (size_t)y * rowbytes;

      if (use_rows != 0)
      {
         for (y = 0; y < image.height; ++y)
            png_read_row(png_ptr, rows[y], NULL);
      }

      else
         png_read_image(png_ptr, rows);

      free(rows);
   }

   png_read_end(png_ptr, NULL);
   png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
   fclose(file);
   return image;
}

static double
overlap(double start, double end, unsigned int pixel)
{
   double low = start > (double)pixel ? start : (double)pixel;
   double high = end < (double)(pixel + 1) ? end : (double)(pixel + 1);
   return high > low ? high - low : 0;
}

static test_image
reference_downsample(const test_image *source, png_uint_32 width,
    png_uint_32 height)
{
   test_image target;
   size_t rowbytes = (size_t)width * source->channels *
       (source->bit_depth == 16 ? 2 : 1);
   png_uint_32 x, y;

   memset(&target, 0, sizeof target);
   target.width = width;
   target.height = height;
   target.channels = source->channels;
   target.bit_depth = source->bit_depth;
   target.data = (png_byte *)calloc(rowbytes, height);
   if (target.data == NULL)
      exit(2);

   for (y = 0; y < height; ++y)
   {
      double y0 = (double)y * source->height / height;
      double y1 = (double)(y + 1) * source->height / height;
      unsigned int sy0 = (unsigned int)y0;
      unsigned int sy1 = (unsigned int)y1;

      if ((double)sy1 < y1)
         ++sy1;

      for (x = 0; x < width; ++x)
      {
         double x0 = (double)x * source->width / width;
         double x1 = (double)(x + 1) * source->width / width;
         unsigned int sx0 = (unsigned int)x0;
         unsigned int sx1 = (unsigned int)x1;
         unsigned int c, sx, sy;

         if ((double)sx1 < x1)
            ++sx1;

         for (c = 0; c < source->channels; ++c)
         {
            double sum = 0;
            double weight = 0;

            for (sy = sy0; sy < sy1; ++sy)
            {
               double wy = overlap(y0, y1, sy);
               if (wy == 0)
                  continue;

               for (sx = sx0; sx < sx1; ++sx)
               {
                  double wx = overlap(x0, x1, sx);
                  double value;

                  if (wx == 0)
                     continue;

                  if (source->bit_depth == 8)
                     value = source->data[
                         ((size_t)sy * source->width + sx) *
                         source->channels + c];

                  else
                  {
                     const png_byte *bp = source->data +
                         (((size_t)sy * source->width + sx) *
                         source->channels + c) * 2;

                     if (source->swapped != 0)
                        value = (double)((bp[1] << 8) | bp[0]);

                     else
                        value = (double)((bp[0] << 8) | bp[1]);
                  }

                  sum += value * wx * wy;
                  weight += wx * wy;
               }
            }

            {
               unsigned int value = (unsigned int)(sum / weight + 0.5);

               if (source->bit_depth == 8)
                  target.data[
                      ((size_t)y * width + x) * source->channels + c] =
                      (png_byte)value;

               else
               {
                  png_byte *bp = target.data +
                      (((size_t)y * width + x) * source->channels + c) * 2;

                  if (source->swapped != 0)
                  {
                     bp[0] = (png_byte)(value & 255);
                     bp[1] = (png_byte)(value >> 8);
                  }

                  else
                  {
                     bp[0] = (png_byte)(value >> 8);
                     bp[1] = (png_byte)(value & 255);
                  }
               }
            }
         }
      }
   }

   return target;
}

static void
compare_case_ex(const char *name, const char *file, png_uint_32 width,
    png_uint_32 height, int swap)
{
   test_image source = read_png(file, 0, 0, 0, swap);
   test_image actual = read_png(file, width, height, 0, swap);
   test_image expected;
   size_t rowbytes;
   size_t i;
   unsigned int maxdiff = 0;

   if (source.data == NULL || actual.data == NULL)
   {
      fprintf(stderr, "%s: decode failed\n", name);
      ++failures;
      return;
   }

   if (actual.width != width || actual.height != height)
   {
      fprintf(stderr, "%s: wrong output dimensions\n", name);
      ++failures;
      return;
   }

   expected = reference_downsample(&source, width, height);
   rowbytes = (size_t)width * actual.channels *
       (actual.bit_depth == 16 ? 2 : 1);

   for (i = 0; i < rowbytes * height; ++i)
   {
      unsigned int diff = actual.data[i] > expected.data[i] ?
          actual.data[i] - expected.data[i] :
          expected.data[i] - actual.data[i];

      if (diff > maxdiff)
         maxdiff = diff;
   }

   printf("%s: %ux%u -> %ux%u, maxdiff=%u\n", name, source.width,
       source.height, width, height, maxdiff);
   if (maxdiff > 1)
      ++failures;

   free(source.data);
   free(actual.data);
   free(expected.data);
}

static void
compare_case(const char *name, const char *file, png_uint_32 width,
    png_uint_32 height)
{
   compare_case_ex(name, file, width, height, 0);
}

static void
test_corrupt(const char *source_name)
{
   FILE *file = fopen(source_name, "rb");
   long size;
   png_byte *data;
   png_byte output[64];
   png_byte expected[64];
   png_struct *png_ptr;
   png_info *info_ptr;
   int failed = 0;

   if (file == NULL)
      exit(2);

   fseek(file, 0, SEEK_END);
   size = ftell(file);
   fseek(file, 0, SEEK_SET);
   data = (png_byte *)malloc((size_t)size);
   if (data == NULL || fread(data, 1, (size_t)size, file) != (size_t)size)
      exit(2);
   fclose(file);

   size = size * 2 / 3;

   file = fopen("pngdownsample-corrupt.png", "wb");
   if (file == NULL)
      exit(2);
   fwrite(data, 1, (size_t)size, file);
   fclose(file);
   free(data);

   memset(output, 0xa5, sizeof output);
   memcpy(expected, output, sizeof output);

   file = fopen("pngdownsample-corrupt.png", "rb");
   png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL,
       test_error_quiet, test_warning);
   info_ptr = png_create_info_struct(png_ptr);
   if (png_ptr == NULL || info_ptr == NULL)
      exit(2);

   if (setjmp(png_jmpbuf(png_ptr)) != 0)
      failed = 1;

   if (failed == 0)
   {
      png_init_io(png_ptr, file);
      png_read_info(png_ptr, info_ptr);
      png_set_downsample(png_ptr, 7, 5);
      png_read_update_info(png_ptr, info_ptr);
      png_read_row(png_ptr, output, NULL);
   }

   if (failed == 0 || memcmp(output, expected, sizeof output) != 0)
   {
      fprintf(stderr, "corrupt input exposed partial output\n");
      ++failures;
   }

   png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
   fclose(file);
   remove("pngdownsample-corrupt.png");
}

#endif /* READ_DOWNSAMPLE */

int
main(void)
{
#ifdef PNG_READ_DOWNSAMPLE_SUPPORTED
   static const char *files[] =
   {
      "pngdownsample-rgb.png",
      "pngdownsample-rgb-i.png",
      "pngdownsample-pal.png",
      "pngdownsample-pal-i.png",
      "pngdownsample-16.png",
      "pngdownsample-16-i.png"
   };
   size_t i;

   write_png(files[0], 37, 29, PNG_COLOR_TYPE_RGB, 8,
       PNG_INTERLACE_NONE, 0, 0);
   write_png(files[1], 37, 29, PNG_COLOR_TYPE_RGB, 8,
       PNG_INTERLACE_ADAM7, 0, 0);
   write_png(files[2], 31, 23, PNG_COLOR_TYPE_PALETTE, 8,
       PNG_INTERLACE_NONE, 1, 1);
   write_png(files[3], 31, 23, PNG_COLOR_TYPE_PALETTE, 8,
       PNG_INTERLACE_ADAM7, 1, 1);
   write_png(files[4], 23, 17, PNG_COLOR_TYPE_RGBA, 16,
       PNG_INTERLACE_NONE, 0, 0);
   write_png(files[5], 23, 17, PNG_COLOR_TYPE_RGBA, 16,
       PNG_INTERLACE_ADAM7, 0, 0);

   compare_case("rgb", files[0], 13, 7);
   compare_case("rgb-adam7", files[1], 13, 7);
   compare_case("palette-trns", files[2], 11, 5);
   compare_case("palette-trns-adam7", files[3], 11, 5);
   compare_case("rgba16", files[4], 9, 4);
   compare_case("rgba16-adam7", files[5], 9, 4);
   compare_case_ex("rgba16-swapped", files[4], 9, 4, 1);
   compare_case_ex("rgba16-adam7-swapped", files[5], 9, 4, 1);

   {
      test_image by_image = read_png(files[1], 13, 7, 0, 0);
      test_image by_rows = read_png(files[1], 13, 7, 1, 0);
      size_t rowbytes = (size_t)by_image.width * by_image.channels;

      if (by_image.data == NULL || by_rows.data == NULL ||
          memcmp(by_image.data, by_rows.data, rowbytes * by_image.height) != 0 ||
          status_calls == 0 || status_passes != 0x7f)
      {
         fprintf(stderr, "row API or status callback failed\n");
         ++failures;
      }

      free(by_image.data);
      free(by_rows.data);
   }

   test_corrupt(files[1]);

   for (i = 0; i < sizeof files / sizeof files[0]; ++i)
      remove(files[i]);

   if (failures != 0)
   {
      fprintf(stderr, "%d downsampling test(s) failed\n", failures);
      return 1;
   }

   puts("downsampling tests passed");
   return 0;
#else
   puts("downsampling not supported in this build");
   return 0;
#endif
}
