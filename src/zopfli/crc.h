#ifndef ZOPFLI_CRC_H_
#define ZOPFLI_CRC_H_

#ifdef __cplusplus
extern "C" {
#endif

#define INIT_CRC_TABLE_MANUALLY

unsigned long UpdateCRC(unsigned long crc, const unsigned char *buf, size_t len);
unsigned long lodepng_crc32(const unsigned char* buf, size_t len);
#ifdef INIT_CRC_TABLE_MANUALLY
void MakeCRCTable(void);
#endif

#ifdef __cplusplus
}
#endif
#endif