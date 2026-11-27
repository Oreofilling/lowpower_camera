/* Stub for CONFIG_CAMERA_JPEG_CONVERTER=n: keeps img_converters.h linkable
 * (liveview calls this for non-JPEG pixel formats) while excluding the ~6-9K
 * software encoder tables from .bss. */
#include <stdbool.h>
#include <stddef.h>
#include "img_converters.h"

bool frame2jpg(camera_fb_t *fb, uint8_t quality, uint8_t **buf, size_t *len)
{
    (void)fb;
    (void)quality;
    (void)buf;
    (void)len;
    return false;
}

bool frame2jpg_cb(camera_fb_t *fb, uint8_t quality, jpg_out_cb cb, void *arg)
{
    (void)fb;
    (void)quality;
    (void)cb;
    (void)arg;
    return false;
}
