#ifndef ZOPFLI_FTOL_H_
#define ZOPFLI_FTOL_H_

typedef union {
  double l_d;
  long l_l;
} ftol_magic;
#define FTOL_MAGIC_NUMBER 6755399441055744.0
#define ftol_magic_convert(f) (((f).l_d += FTOL_MAGIC_NUMBER), (void)0)
#define ftol_magic_getlong(f) ((f).l_l)
#define ftol_magic_setdouble(f, v) ((f).l_d = v)

#endif