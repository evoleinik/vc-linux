/* Browser-only translation registry. The generated Image and CPU contracts
 * are unchanged; native builds still link every translation statically. */
#ifndef VC_WEB_PROGRAMS_H
#define VC_WEB_PROGRAMS_H

#include <stddef.h>
#include "image.h"

enum {
    WEB_VC_COM, WEB_VC_OVL, WEB_GWBASIC, WEB_BOOTLOGO, WEB_ROGUE, WEB_VZ, WEB_KERMIT,
    WEB_COMMAND, WEB_EDLIN, WEB_DEBUG, WEB_FIND, WEB_MORE, WEB_SORT, WEB_FC,
    WEB_IMAGE_COUNT
};
const char *web_image_filename(size_t index);
/* Called only after the complete DOS file matched its embedded reference.
 * May suspend the dispatcher's direct Asyncify chain; never Image.run. */
int web_load_image(size_t index, const Image **out);
int web_image_is_vz(const Image *image);

#endif
