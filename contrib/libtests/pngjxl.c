#include "../../png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void
fail(const char *message)
{
   fprintf(stderr, "pngjxl: %s\n", message);
   ++failures;
}

static void
fill_rgba16(png_uint_16 *pixels, png_uint_32 width, png_uint_32 height)
{
   png_uint_32 y;

   for (y = 0; y < height; ++y)
   {
      png_uint_32 x;

      for (x = 0; x < width; ++x)
      {
         png_uint_16 *p = pixels + ((size_t)y * width + x) * 4;

         p[0] = (png_uint_16)((x * 4051U + y * 977U) & 0xffffU);
         p[1] = (png_uint_16)((x * 1237U + y * 7919U) & 0xffffU);
         p[2] = (png_uint_16)((x * 65521U + y * 31U) & 0xffffU);
         p[3] = (png_uint_16)((x * 13U + y * 17U + 19U) & 0xffffU);
      }
   }
}

static int
write_jxl(png_image *image, const void *pixels,
    const png_image_jxl_options *options, png_alloc_size_t *size,
    void **memory)
{
   png_alloc_size_t capacity = 0;

   *memory = NULL;
   *size = 0;

   if (png_image_write_jxl_to_memory(image, NULL, &capacity, pixels, 0, NULL,
       options) == 0)
      return 0;

   *memory = malloc((size_t)capacity);

   if (*memory == NULL)
      return 0;

   *size = capacity;

   if (png_image_write_jxl_to_memory(image, *memory, size, pixels, 0, NULL,
       options) == 0)
   {
      free(*memory);
      *memory = NULL;
      return 0;
   }

   return 1;
}

static int
read_jxl(const void *memory, size_t size, png_uint_32 format, void **pixels,
    png_byte **icc, size_t *icc_size)
{
   png_image image;
   size_t row_bytes;

   memset(&image, 0, sizeof image);
   image.version = PNG_IMAGE_VERSION;
   *pixels = NULL;
   *icc = NULL;
   *icc_size = 0;

   if (png_image_begin_read_from_memory(&image, memory, size) == 0)
      return 0;

   if ((image.flags & PNG_IMAGE_FLAG_FORMAT_JPEGXL) == 0)
   {
      fail("JPEG XL input was not detected from its contents");
      png_image_free(&image);
      return 0;
   }

   if (png_image_get_icc_profile(&image, NULL, icc_size) == 0 ||
       *icc_size == 0)
   {
      fail("JPEG XL input has no ICC profile");
      png_image_free(&image);
      return 0;
   }

   *icc = malloc(*icc_size);

   if (*icc == NULL ||
       png_image_get_icc_profile(&image, *icc, icc_size) == 0)
   {
      fail("cannot read JPEG XL ICC profile");
      free(*icc);
      *icc = NULL;
      png_image_free(&image);
      return 0;
   }

   image.format = format;
   row_bytes = PNG_IMAGE_ROW_STRIDE(image);
   *pixels = malloc(PNG_IMAGE_SIZE(image));

   if (*pixels == NULL ||
       png_image_finish_read(&image, NULL, *pixels, (png_int_32)row_bytes,
       NULL) == 0)
   {
      free(*pixels);
      *pixels = NULL;
      png_image_free(&image);
      return 0;
   }

   if (png_image_get_icc_profile(&image, NULL, icc_size) == 0 ||
       *icc_size == 0)
   {
      fail("ICC profile was not retained after finish_read");
      png_image_free(&image);
      return 0;
   }

   png_image_free(&image);
   return 1;
}

static void
test_roundtrip(png_uint_32 width, png_uint_32 height, png_uint_32 format,
    const void *source, size_t source_size, const char *description)
{
   png_image writer;
   png_image_jxl_options options;
   png_alloc_size_t jxl_size = 0;
   void *jxl = NULL;
   void *decoded = NULL;
   png_byte *icc = NULL;
   size_t icc_size = 0;

   memset(&writer, 0, sizeof writer);
   writer.version = PNG_IMAGE_VERSION;
   writer.width = width;
   writer.height = height;
   writer.format = format;

   memset(&options, 0, sizeof options);
   options.version = PNG_IMAGE_JXL_OPTIONS_VERSION;
   options.lossless = 1;

   if (write_jxl(&writer, source, &options, &jxl_size, &jxl) == 0)
      fail(description);
   else if (read_jxl(jxl, (size_t)jxl_size, format, &decoded, &icc,
       &icc_size) == 0)
      fail(description);
   else if (memcmp(source, decoded, source_size) != 0)
      fail(description);

   free(icc);
   free(decoded);
   free(jxl);
}

int
main(void)
{
   enum { width = 37, height = 23 };
   png_uint_16 *source = malloc(sizeof(png_uint_16) * width * height * 4);
   png_uint_16 *decoded = NULL;
   png_byte *icc1 = NULL;
   png_byte *icc2 = NULL;
   size_t icc1_size = 0;
   size_t icc2_size = 0;
   void *jxl = NULL;
   png_alloc_size_t jxl_size = 0;
   png_image writer;
   png_image_jxl_options options;

   if (source == NULL)
   {
      fail("out of memory");
      return 1;
   }

   fill_rgba16(source, width, height);

   memset(&writer, 0, sizeof writer);
   writer.version = PNG_IMAGE_VERSION;
   writer.width = width;
   writer.height = height;
   writer.format = PNG_FORMAT_LINEAR_RGB_ALPHA;

   memset(&options, 0, sizeof options);
   options.version = PNG_IMAGE_JXL_OPTIONS_VERSION;
   options.lossless = 1;

   if (write_jxl(&writer, source, &options, &jxl_size, &jxl) == 0)
   {
      fail("cannot encode lossless JPEG XL");
      free(source);
      return 1;
   }

   if (jxl_size < 2 ||
       (memcmp(jxl, "\xff\x0a", 2) != 0 &&
        (jxl_size < 12 ||
         memcmp(jxl, "\0\0\0\x0cJXL \x0d\x0a\x87\x0a", 12) != 0)))
   {
      fail("JPEG XL output does not have a valid signature");
   }

   {
      const char *file_name = "pngjxl-misleading.png";
      png_image file_reader;
      png_byte *file_pixels = NULL;

      if (png_image_write_jxl_to_file(&writer, file_name, source, 0, NULL,
          &options) == 0)
         fail("cannot write JPEG XL file");
      else
      {
         memset(&file_reader, 0, sizeof file_reader);
         file_reader.version = PNG_IMAGE_VERSION;

         if (png_image_begin_read_from_file(&file_reader, file_name) == 0)
            fail("JPEG XL file was rejected because of its extension");
         else
         {
            if ((file_reader.flags & PNG_IMAGE_FLAG_FORMAT_JPEGXL) == 0)
               fail("JPEG XL file content was not detected");

            file_reader.format = PNG_FORMAT_LINEAR_RGB_ALPHA;
            file_pixels = malloc(PNG_IMAGE_SIZE(file_reader));

            if (file_pixels == NULL ||
                png_image_finish_read(&file_reader, NULL, file_pixels, 0,
                    NULL) == 0)
               fail("cannot decode JPEG XL file");
            else if (memcmp(source, file_pixels,
                sizeof(png_uint_16) * width * height * 4) != 0)
               fail("JPEG XL file roundtrip changed pixels");

            png_image_free(&file_reader);
         }
      }

      free(file_pixels);
      (void)remove(file_name);
   }

   if (read_jxl(jxl, (size_t)jxl_size, PNG_FORMAT_LINEAR_RGB_ALPHA,
       (void **)&decoded, &icc1, &icc1_size) == 0)
      fail("cannot decode lossless JPEG XL");
   else if (memcmp(source, decoded, sizeof(png_uint_16) * width * height * 4)
       != 0)
      fail("lossless JPEG XL roundtrip changed pixels");

   {
      png_image image;
      void *roundtrip = NULL;
      png_alloc_size_t roundtrip_size = 0;
      png_byte *expected_icc = NULL;
      size_t expected_icc_size = 0;

      memset(&image, 0, sizeof image);
      image.version = PNG_IMAGE_VERSION;

      if (png_image_begin_read_from_memory(&image, jxl,
          (size_t)jxl_size) == 0)
         fail("cannot reopen JPEG XL for transparent write");
      else
      {
         image.format = PNG_FORMAT_LINEAR_RGB_ALPHA;

         if (png_image_finish_read(&image, NULL, decoded, 0, NULL) == 0)
            fail("cannot decode JPEG XL before transparent write");
         else if (png_image_get_icc_profile(&image, NULL,
             &expected_icc_size) == 0 || expected_icc_size == 0)
            fail("cannot read JPEG XL profile before transparent write");
         else if (png_image_write_to_memory(&image, NULL, &roundtrip_size, 0,
             decoded, 0, NULL) == 0)
            fail("transparent JPEG XL size query failed");
         else
         {
            expected_icc = malloc(expected_icc_size);
            roundtrip = malloc((size_t)roundtrip_size);

            if (expected_icc == NULL ||
                png_image_get_icc_profile(&image, expected_icc,
                    &expected_icc_size) == 0 ||
                roundtrip == NULL ||
                png_image_write_to_memory(&image, roundtrip, &roundtrip_size,
                    0, decoded, 0, NULL) == 0)
               fail("transparent JPEG XL write failed");
            else if (roundtrip_size < 2 ||
                (memcmp(roundtrip, "\xff\x0a", 2) != 0 &&
                 (roundtrip_size < 12 ||
                  memcmp(roundtrip, "\0\0\0\x0cJXL \x0d\x0a\x87\x0a", 12) !=
                      0)))
               fail("transparent write did not produce JPEG XL");
            else
            {
               void *roundtrip_pixels = NULL;
               png_byte *roundtrip_icc = NULL;
               size_t roundtrip_icc_size = 0;

               if (read_jxl(roundtrip, (size_t)roundtrip_size,
                   PNG_FORMAT_LINEAR_RGB_ALPHA, &roundtrip_pixels,
                   &roundtrip_icc, &roundtrip_icc_size) == 0)
                  fail("cannot decode transparent JPEG XL output");
               else
               {
                  if (memcmp(decoded, roundtrip_pixels,
                      sizeof(png_uint_16) * width * height * 4) != 0)
                     fail("transparent JPEG XL write changed pixels");

                  if (roundtrip_icc_size != expected_icc_size ||
                      memcmp(expected_icc, roundtrip_icc,
                          expected_icc_size) != 0)
                     fail("transparent JPEG XL write changed the ICC profile");
               }

               free(roundtrip_pixels);
               free(roundtrip_icc);
            }
         }
      }

      free(expected_icc);
      free(roundtrip);
      png_image_free(&image);
   }

   free(decoded);
   decoded = NULL;
   free(jxl);
   jxl = NULL;

   options.icc_profile = icc1;
   options.icc_profile_size = icc1_size;

   if (write_jxl(&writer, source, &options, &jxl_size, &jxl) == 0)
      fail("cannot re-encode JPEG XL with a preserved ICC profile");
   else if (read_jxl(jxl, (size_t)jxl_size, PNG_FORMAT_LINEAR_RGB_ALPHA,
       (void **)&decoded, &icc2, &icc2_size) == 0)
      fail("cannot decode JPEG XL after ICC roundtrip");
   else
   {
      if (memcmp(source, decoded, sizeof(png_uint_16) * width * height * 4)
          != 0)
         fail("ICC roundtrip changed pixels");

      if (icc1_size != icc2_size ||
          memcmp(icc1, icc2, icc1_size) != 0)
         fail("ICC profile changed across roundtrip");
   }

   free(decoded);
   decoded = NULL;
   free(jxl);
   jxl = NULL;
   free(icc2);
   icc2 = NULL;

   options.icc_profile = NULL;
   options.icc_profile_size = 0;
   options.lossless = 0;
   options.quality = 85;

   if (write_jxl(&writer, source, &options, &jxl_size, &jxl) == 0)
      fail("cannot encode quality-mode JPEG XL");
   else if (read_jxl(jxl, (size_t)jxl_size, PNG_FORMAT_LINEAR_RGB_ALPHA,
       (void **)&decoded, &icc2, &icc2_size) == 0)
      fail("cannot decode quality-mode JPEG XL");

   free(decoded);
   decoded = NULL;
   free(jxl);
   jxl = NULL;

   {
      static const png_byte unknown[16] =
          { 0x89, 'N', 'O', 'T', 'P', 'N', 'G', 0x0d, 0, 1, 2, 3, 4, 5, 6, 7 };
      png_image image;

      memset(&image, 0, sizeof image);
      image.version = PNG_IMAGE_VERSION;
      (void)png_image_begin_read_from_memory(&image, unknown,
          sizeof unknown);

      if (png_image_get_error_class(&image) !=
          PNG_IMAGE_ERROR_CLASS_UNSUPPORTED_FORMAT)
         fail("unknown format was not reported as unsupported");
   }

   {
      png_image_jxl_options lossless_options;
      png_alloc_size_t valid_size = 0;
      void *valid = NULL;
      png_image image;
      png_uint_16 pixel[width * height * 4];

      memset(&lossless_options, 0, sizeof lossless_options);
      lossless_options.version = PNG_IMAGE_JXL_OPTIONS_VERSION;
      lossless_options.lossless = 1;

      if (write_jxl(&writer, source, &lossless_options, &valid_size,
          &valid) == 0)
         fail("cannot create corrupt-data test input");

      memset(&image, 0, sizeof image);
      image.version = PNG_IMAGE_VERSION;

      if (valid != NULL &&
          png_image_begin_read_from_memory(&image, valid,
              (size_t)valid_size / 2) != 0)
      {
         image.format = PNG_FORMAT_LINEAR_RGB_ALPHA;
         (void)png_image_finish_read(&image, NULL, pixel, 0, NULL);
      }

      if (png_image_get_error_class(&image) !=
          PNG_IMAGE_ERROR_CLASS_CORRUPT_DATA)
         fail("corrupt JPEG XL was not reported as corrupt data");

      free(valid);
   }

   {
      png_byte rgba[width * height * 4];
      png_image png_writer;
      png_image png_reader;
      png_alloc_size_t png_size = 0;
      void *png_memory = NULL;
      png_byte *png_pixels = NULL;
      png_uint_32 y;

      for (y = 0; y < height; ++y)
      {
         png_uint_32 x;

         for (x = 0; x < width; ++x)
         {
            png_byte *p = rgba + ((size_t)y * width + x) * 4;

            p[0] = (png_byte)(x + y);
            p[1] = (png_byte)(x * 3 + y);
            p[2] = (png_byte)(x + y * 7);
            p[3] = (png_byte)(x * 5 + y * 11);
         }
      }

      memset(&png_writer, 0, sizeof png_writer);
      png_writer.version = PNG_IMAGE_VERSION;
      png_writer.width = width;
      png_writer.height = height;
      png_writer.format = PNG_FORMAT_RGBA;

      if (png_image_write_to_memory(&png_writer, NULL, &png_size, 0, rgba,
          0, NULL) == 0)
         fail("PNG size query failed");
      else
      {
         png_memory = malloc((size_t)png_size);

         if (png_memory == NULL ||
             png_image_write_to_memory(&png_writer, png_memory, &png_size, 0,
                 rgba, 0, NULL) == 0)
            fail("PNG write failed");
      }

      if (png_memory != NULL)
      {
         memset(&png_reader, 0, sizeof png_reader);
         png_reader.version = PNG_IMAGE_VERSION;

         if (png_image_begin_read_from_memory(&png_reader, png_memory,
             (size_t)png_size) == 0)
            fail("PNG content detection failed");
         else
         {
            png_reader.format = PNG_FORMAT_RGBA;
            png_pixels = malloc(PNG_IMAGE_SIZE(png_reader));

            if (png_pixels == NULL ||
                png_image_finish_read(&png_reader, NULL, png_pixels, 0,
                    NULL) == 0)
               fail("PNG roundtrip failed");
            else if (memcmp(rgba, png_pixels, sizeof rgba) != 0)
               fail("PNG roundtrip changed pixels");
         }
      }

      if (png_memory != NULL)
      {
         const char *offset_name = "pngjxl-offset.png";
         FILE *file = fopen(offset_name, "wb");

         if (file == NULL || fwrite("abc", 1, 3, file) != 3 ||
             fwrite(png_memory, 1, (size_t)png_size, file) !=
                 (size_t)png_size ||
             fclose(file) != 0)
            fail("cannot create offset PNG test file");
         else
         {
            file = fopen(offset_name, "rb");

            if (file == NULL || fseek(file, 3, SEEK_SET) != 0)
               fail("cannot position offset PNG test file");
            else
            {
               memset(&png_reader, 0, sizeof png_reader);
               png_reader.version = PNG_IMAGE_VERSION;

               if (png_image_begin_read_from_stdio(&png_reader, file) == 0)
                  fail("PNG at a nonzero stream offset was not detected");
               else
               {
                  png_reader.format = PNG_FORMAT_RGBA;
                  free(png_pixels);
                  png_pixels = malloc(PNG_IMAGE_SIZE(png_reader));

                  if (png_pixels == NULL ||
                      png_image_finish_read(&png_reader, NULL, png_pixels, 0,
                          NULL) == 0)
                     fail("PNG at a nonzero stream offset failed to decode");
                  else if (memcmp(rgba, png_pixels, sizeof rgba) != 0)
                     fail("PNG at a nonzero offset changed pixels");
               }
            }

            if (file != NULL)
               (void)fclose(file);
         }

         (void)remove(offset_name);
      }

      free(png_pixels);
      free(png_memory);
   }

   {
      png_uint_16 gray[width * height];
      png_byte rgba8[width * height * 4];
      png_uint_32 y;

      for (y = 0; y < height; ++y)
      {
         png_uint_32 x;

         for (x = 0; x < width; ++x)
         {
            png_byte *p = rgba8 + ((size_t)y * width + x) * 4;

            gray[(size_t)y * width + x] =
                (png_uint_16)((x * 4093U + y * 2053U) & 0xffffU);
            p[0] = (png_byte)(x * 11 + y);
            p[1] = (png_byte)(x + y * 13);
            p[2] = (png_byte)(x * 7 + y * 5);
            p[3] = (png_byte)(x * 17 + y * 3);
         }
      }

      test_roundtrip(width, height, PNG_FORMAT_LINEAR_Y, gray, sizeof gray,
          "16-bit grayscale JPEG XL roundtrip failed");
      test_roundtrip(width, height, PNG_FORMAT_RGBA, rgba8, sizeof rgba8,
          "8-bit RGBA JPEG XL roundtrip failed");
   }

   free(icc1);
   free(icc2);
   free(decoded);
   free(jxl);
   free(source);

   if (failures != 0)
   {
      fprintf(stderr, "pngjxl: %d failure(s)\n", failures);
      return 1;
   }

   return 0;
}
