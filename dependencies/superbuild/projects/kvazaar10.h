/* Bloom: the public header of the bundled 10-bit Kvazaar 2.3.2 (kvazaar.cmake), installed as
 * <kvazaar10/kvazaar10.h> beside an unmodified copy of Kvazaar's kvazaar.h. It selects the 10-bit
 * sample type (kvz_pixel is uint16_t) and the kvz10_ symbol names the library was built with, so
 * kvz_api_get() here reaches the 10-bit archive and never the 8-bit one beside it.
 */
#ifndef BLOOM_KVAZAAR10_H
#define BLOOM_KVAZAAR10_H

#define KVZ_BIT_DEPTH 10
#ifndef KVZ_STATIC_LIB
#define KVZ_STATIC_LIB
#endif
#include "kvazaar10-symbols.h"
#include "kvazaar.h"

#endif
