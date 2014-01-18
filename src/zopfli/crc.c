#include "crc.h"

/* Table of CRCs of all 8-bit messages. */
static unsigned long crc_table[256];

/* Makes the table for a fast CRC. */
#if !defined(INIT_CRC_TABLE_MANUALLY)
static
#endif
void MakeCRCTable()
#if defined(__GNUC__)
__attribute__ ((constructor))
#endif
{
  unsigned long c;
  int n, k;
  for (n = 0; n < 256; n++) {
    c = (unsigned long) n;
    for (k = 0; k < 8; k++) {
      if (c & 1) {
        c = 0xedb88320L ^ (c >> 1);
      } else {
        c = c >> 1;
      }
    }
    crc_table[n] = c;
  }
}

#if !defined(__GNUC__) && !defined(INIT_CRC_TABLE_MANUALLY)
/* Flag: To create CRC table on startup */
static int crc_table_computed = 0;
#endif

/*
Updates a running crc with the bytes buf[0..len-1] and returns
the updated crc. The crc should be initialized to zero.
*/
#ifdef _MSC_VER
__forceinline
#elif defined(__GNUC__)
__inline
#endif
unsigned long UpdateCRC(unsigned long crc,
                               const unsigned char *buf, size_t len) {
  unsigned long c = crc ^ 0xffffffffL;
  unsigned n;

#if !defined(__GNUC__) && !defined(INIT_CRC_TABLE_MANUALLY)
  if (!crc_table_computed)
    MakeCRCTable();
#endif
  for (n = 0; n < len; n++) {
    c = crc_table[(c ^ buf[n]) & 0xff] ^ (c >> 8);
  }
  return c ^ 0xffffffffL;
}

/* Returns the CRC of the bytes buf[0..len-1]. */
unsigned lodepng_crc32(const unsigned char* buf, size_t len) {
  return UpdateCRC(0L, buf, len);
}