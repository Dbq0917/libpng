#include "../../png.h"

#include <stdio.h>
#include <string.h>

int
main(void)
{
   static const png_byte jxl[32] =
   {
      0x00, 0x00, 0x00, 0x0c, 'J', 'X', 'L', ' ', 0x0d, 0x0a, 0x87, 0x0a,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
   };
   png_image image;

   memset(&image, 0, sizeof image);
   image.version = PNG_IMAGE_VERSION;

   if (png_image_begin_read_from_memory(&image, jxl, sizeof jxl) != 0)
   {
      fprintf(stderr, "pngjxl-unsupported: JPEG XL unexpectedly accepted\n");
      return 1;
   }

   if (png_image_get_error_class(&image) !=
       PNG_IMAGE_ERROR_CLASS_UNSUPPORTED_FORMAT)
   {
      fprintf(stderr,
          "pngjxl-unsupported: JPEG XL error was not classified as unsupported\n");
      return 1;
   }

   {
      static const png_byte pixel[4] = { 1, 2, 3, 4 };
      png_alloc_size_t size = 0;

      memset(&image, 0, sizeof image);
      image.version = PNG_IMAGE_VERSION;
      image.width = 1;
      image.height = 1;
      image.format = PNG_FORMAT_RGBA;

      if (png_image_write_jxl_to_memory(&image, NULL, &size, pixel, 0, NULL,
          NULL) != 0 ||
          png_image_get_error_class(&image) !=
              PNG_IMAGE_ERROR_CLASS_UNSUPPORTED_FORMAT)
      {
         fprintf(stderr,
             "pngjxl-unsupported: JPEG XL write was not rejected\n");
         return 1;
      }
   }

   return 0;
}
