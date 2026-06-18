/*
 * jmem_psram.c - CDC Badge OS system-dependent JPEG memory manager.
 *
 * This is the project's own implementation of libjpeg's system-dependent
 * memory interface (jmemsys.h), used instead of the upstream jmemnobs.c so the
 * upstream sources stay pristine. All allocations are served from PSRAM
 * (MALLOC_CAP_SPIRAM); internal RAM is the scarce pool on this device and
 * progressive JPEG decode needs large coefficient buffers. No backing store;
 * max_memory_to_use is respected.
 */

#define JPEG_INTERNALS
#include "jinclude.h"
#include "jpeglib.h"
#include "jmemsys.h"

#include "esp_heap_caps.h"

GLOBAL(void *)
jpeg_get_small (j_common_ptr cinfo, size_t sizeofobject)
{
  return (void *) heap_caps_malloc(sizeofobject, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

GLOBAL(void)
jpeg_free_small (j_common_ptr cinfo, void * object, size_t sizeofobject)
{
  heap_caps_free(object);
}

GLOBAL(void FAR *)
jpeg_get_large (j_common_ptr cinfo, size_t sizeofobject)
{
  return (void FAR *) heap_caps_malloc(sizeofobject, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

GLOBAL(void)
jpeg_free_large (j_common_ptr cinfo, void FAR * object, size_t sizeofobject)
{
  heap_caps_free(object);
}

GLOBAL(long)
jpeg_mem_available (j_common_ptr cinfo, long min_bytes_needed,
		    long max_bytes_needed, long already_allocated)
{
  if (cinfo->mem->max_memory_to_use)
    return cinfo->mem->max_memory_to_use - already_allocated;
  return max_bytes_needed;
}

GLOBAL(void)
jpeg_open_backing_store (j_common_ptr cinfo, backing_store_ptr info,
			 long total_bytes_needed)
{
  ERREXIT(cinfo, JERR_NO_BACKING_STORE);
}

GLOBAL(long)
jpeg_mem_init (j_common_ptr cinfo)
{
  return 0;
}

GLOBAL(void)
jpeg_mem_term (j_common_ptr cinfo)
{
}
