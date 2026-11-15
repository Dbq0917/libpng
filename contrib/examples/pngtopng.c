/*- pngtopng
 *
 * COPYRIGHT: Written by John Cunningham Bowler, 2011, 2017.
 * To the extent possible under law, the author has waived all copyright and
 * related or neighboring rights to this work.  This work is published from:
 * United States.
 *
 * Read a PNG and write it out as RGBA, preserving the source bit depth.
 * This avoids the 16-to-8-bit reduction and alpha premultiplication done by
 * the simplified API when it converts an image to sRGB RGBA.
 *
 * See example.c in the top-level libpng directory for a simplified API
 * example.  This program uses the regular read/write interface so that the
 * pixels, including the RGB values of transparent pixels, are preserved.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* Normally use <png.h> here to get the installed libpng, but this is done to
 * ensure the code picks up the local libpng implementation:
 */
#include "../../png.h"

#if !defined(PNG_READ_EXPAND_SUPPORTED) || \
    !defined(PNG_READ_FILLER_SUPPORTED) || \
    !defined(PNG_READ_GRAY_TO_RGB_SUPPORTED)
#error This program requires the standard PNG read transformations
#endif


static int
copy_png(const char *input_name, const char *output_name)
{
   FILE *input = fopen(input_name, "rb");
   FILE *output = NULL;
   png_struct *read_ptr = NULL;
   png_info *info_ptr = NULL;
   png_struct *write_ptr = NULL;
   png_byte *row_ptr = NULL;
   volatile png_byte *row = NULL;
   int passes;
   int result = 0;

   if (input == NULL)
   {
      fprintf(stderr, "pngtopng: read %s: cannot open file\n", input_name);
      return 0;
   }

   output = fopen(output_name, "wb");
   if (output == NULL)
   {
      fprintf(stderr, "pngtopng: write %s: cannot open file\n", output_name);
      fclose(input);
      return 0;
   }

   read_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
   if (read_ptr != NULL)
      info_ptr = png_create_info_struct(read_ptr);

   write_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL,
       NULL);

   if (read_ptr == NULL || info_ptr == NULL || write_ptr == NULL)
   {
      fprintf(stderr, "pngtopng: out of memory\n");
      goto cleanup;
   }

   if (setjmp(png_jmpbuf(read_ptr)))
   {
      fprintf(stderr, "pngtopng: read %s: invalid PNG data\n", input_name);
      goto cleanup;
   }

   png_init_io(read_ptr, input);
   png_read_info(read_ptr, info_ptr);

   {
      const int bit_depth = png_get_bit_depth(read_ptr, info_ptr);
      const int color_type = png_get_color_type(read_ptr, info_ptr);
      const int has_alpha =
          (color_type & PNG_COLOR_MASK_ALPHA) != 0 ||
          png_get_valid(read_ptr, info_ptr, PNG_INFO_tRNS) != 0;

      /* Expand palettes, tRNS and sub-byte grayscale without touching the
       * sample values of an 8-bit or 16-bit truecolour image.
       */
      png_set_expand(read_ptr);

      if ((color_type & PNG_COLOR_MASK_COLOR) == 0)
         png_set_gray_to_rgb(read_ptr);

      if (!has_alpha)
         png_set_add_alpha(read_ptr, bit_depth == 16 ? 0xffffU : 0xffU,
             PNG_FILLER_AFTER);

      passes = png_set_interlace_handling(read_ptr);
      png_read_update_info(read_ptr, info_ptr);

      /* The output is always truecolour RGBA.  Do not copy palette-only
       * metadata into the new IHDR context; in particular, a stale tRNS chunk
       * cannot coexist with an alpha channel.
       */
      png_free_data(read_ptr, info_ptr,
          PNG_FREE_PLTE | PNG_FREE_TRNS | PNG_FREE_HIST, 0);

      if (png_get_color_type(read_ptr, info_ptr) !=
              PNG_COLOR_TYPE_RGB_ALPHA ||
          (png_get_bit_depth(read_ptr, info_ptr) != 8 &&
           png_get_bit_depth(read_ptr, info_ptr) != 16))
      {
         fprintf(stderr, "pngtopng: read %s: unsupported PNG colour type\n",
             input_name);
         goto cleanup;
      }
   }

   if (setjmp(png_jmpbuf(write_ptr)))
   {
      fprintf(stderr, "pngtopng: write %s: failed to encode PNG data\n",
          output_name);
      goto cleanup;
   }

   png_init_io(write_ptr, output);

   /* Keep the output compact; the source bit depth, rather than a lower
    * common denominator, determines the precision of the result.
    */
#ifdef PNG_WRITE_CUSTOMIZE_COMPRESSION_SUPPORTED
   png_set_compression_level(write_ptr, 9);
#endif
   png_write_info(write_ptr, info_ptr);
   png_set_interlace_handling(write_ptr);

   {
      const size_t rowbytes = png_get_rowbytes(read_ptr, info_ptr);
      int pass;
      png_uint_32 y;

      row_ptr = malloc(rowbytes);
      row = row_ptr;
      if (row_ptr == NULL)
      {
         fprintf(stderr, "pngtopng: out of memory\n");
         goto cleanup;
      }

      for (pass = 0; pass < passes; ++pass)
      {
         for (y = 0; y < png_get_image_height(read_ptr, info_ptr); ++y)
         {
            png_read_row(read_ptr, row_ptr, NULL);
            png_write_row(write_ptr, row_ptr);
         }
      }
   }

   png_read_end(read_ptr, info_ptr);
   png_write_end(write_ptr, info_ptr);
   result = 1;

cleanup:
   free((void *)row);
   png_destroy_write_struct(&write_ptr, NULL);
   png_destroy_read_struct(&read_ptr, &info_ptr, NULL);

   if (fclose(output) != 0)
      result = 0;

   fclose(input);

   if (!result)
      remove(output_name);

   return result;
}


int
main(int argc, const char **argv)
{
   if (argc == 3)
      return copy_png(argv[1], argv[2]) ? 0 : 1;

   fprintf(stderr, "pngtopng: usage: pngtopng input-file output-file\n");
   return 1;
}
