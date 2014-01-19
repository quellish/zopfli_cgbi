/*
Copyright 2011 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

Author: lode.vandevenne@gmail.com (Lode Vandevenne)
Author: jyrki.alakuijala@gmail.com (Jyrki Alakuijala)
*/

#include "cache.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef ZOPFLI_LONGEST_MATCH_CACHE

void ZopfliInitCache(size_t blocksize, ZopfliLongestMatchCache* lmc) {
  size_t i;
  lmc->length = (unsigned short*)malloc(sizeof(unsigned short) * blocksize);
  lmc->dist = (unsigned short*)malloc(sizeof(unsigned short) * blocksize);
  /* Rather large amount of memory. */
  lmc->sublen = (unsigned char*)malloc(ZOPFLI_CACHE_LENGTH * 3 * blocksize);

  /* length > 0 and dist 0 is invalid combination, which indicates on purpose
  that this cache value is not filled in yet. */
  for (i = 0; i < blocksize; i++) lmc->length[i] = 1;
  for (i = 0; i < blocksize; i++) lmc->dist[i] = 0;
  for (i = 0; i < ZOPFLI_CACHE_LENGTH * blocksize * 3; i++) lmc->sublen[i] = 0;
}

#if !defined(__GNUC__) && !defined(_MSC_VER)
void ZopfliCleanCache(ZopfliLongestMatchCache* lmc) {
  free(lmc->length);
  free(lmc->dist);
  free(lmc->sublen);
}
#endif

#if 1 && defined(_MSC_VER) && !defined(DEBUG) && !defined(_DEBUG)
__declspec(naked)
void ZopfliSublenToCache(const unsigned short* sublen,
                         size_t pos, size_t length,
                         ZopfliLongestMatchCache* lmc) {
__asm{
	push edi
	push esi
	xor edx, edx
	mov edi, [esp+20]
	lea esi, [edx+8]
	sub edi, 3
	jb func_end
	push ebx
	push ebp
	mov eax, [esp+32]
	mov ecx, [esp+24]
	mov ebp, [esp+20]
	mov ebx, [eax+8]
	lea ecx, [ecx+2*ecx]
	movzx eax, word ptr [ebp+6]
	/*__asm __emit 0x8D __asm __emit 0x4C __asm __emit 0xCB __asm __emit 0x00*/
	lea ecx, [ebx+8*ecx+0]
	mov [ecx+21], dl
	je fillsublen
loopnext:
	movzx ebx, word ptr [ebp+2*edx+8]
	cmp eax, ebx
	je skipfillsublen
fillsublen:
	mov [ecx], dl
    dec esi
	lea ecx, [ecx+3]
	mov [ecx-2], ax
	jz loopend
skipfillsublen:
	inc edx
	xchg eax, ebx
	cmp edx, edi
	jb loopnext
	je fillsublen
	cmp esi, 8
	je skipreadal /* cmov not ok due to memory boundary */
	mov al, [ecx-3]
	add ecx, esi
	mov [ecx+2*esi-3], al
skipreadal:
loopend:
	pop ebp
	pop ebx
func_end:
	pop esi
	pop edi
	ret
}
}
#else
void ZopfliSublenToCache(const unsigned short* sublen,
                         size_t pos, size_t length,
                         ZopfliLongestMatchCache* lmc) {
  size_t i;
  size_t j = 0;
  unsigned bestlength = 0;
  unsigned char* cache;

#if ZOPFLI_CACHE_LENGTH == 0
  return;
#endif

  cache = &lmc->sublen[ZOPFLI_CACHE_LENGTH * pos * 3];
  if (length < 3) return;
  for (i = 3; i <= length; i++) {
    if (i == length || sublen[i] != sublen[i + 1]) {
      cache[j * 3] = i - 3;
      cache[j * 3 + 1] = sublen[i] % 256;
      cache[j * 3 + 2] = (sublen[i] >> 8) % 256;
      bestlength = i;
      j++;
      if (j >= ZOPFLI_CACHE_LENGTH) break;
    }
  }
  if (j < ZOPFLI_CACHE_LENGTH) {
    assert(bestlength == length);
    cache[(ZOPFLI_CACHE_LENGTH - 1) * 3] = bestlength - 3;
  } else {
    assert(bestlength <= length);
  }
  assert(bestlength == ZopfliMaxCachedSublen(lmc, pos, length));
}
#endif

/*
Returns the length up to which could be stored in the cache.
*/
static unsigned _ZopfliMaxCachedSublen(unsigned char* cache) {
  if (cache[1] == 0 && cache[2] == 0) return 0;  /* No sublen cached. */
  return cache[(ZOPFLI_CACHE_LENGTH - 1) * 3] + 3;
}

void ZopfliCacheToSublen(const ZopfliLongestMatchCache* lmc,
                         size_t pos, size_t length,
                         unsigned short* sublen) {
  size_t i, j;
  unsigned maxlength;
  unsigned prevlength;
  unsigned char* cache;
#if ZOPFLI_CACHE_LENGTH == 0
  return;
#endif
  if (length < 3) return;
  cache = &lmc->sublen[ZOPFLI_CACHE_LENGTH * pos * 3];
  maxlength = _ZopfliMaxCachedSublen(cache);
  prevlength = 0;
  for (j = 0; j < ZOPFLI_CACHE_LENGTH; j++) {
    unsigned length = cache[j * 3] + 3;
    unsigned dist = cache[j * 3 + 1] | cache[j * 3 + 2] << 8;
    for (i = prevlength; i <= length; i++) {
      sublen[i] = dist;
    }
    if (length == maxlength) break;
    prevlength = length + 1;
  }
}

/*
Returns the length up to which could be stored in the cache.
*/
unsigned ZopfliMaxCachedSublen(const ZopfliLongestMatchCache* lmc,
                               size_t pos, size_t length) {
  unsigned char* cache;
#if ZOPFLI_CACHE_LENGTH == 0
  return 0;
#endif
  cache = &lmc->sublen[ZOPFLI_CACHE_LENGTH * pos * 3];
  (void)length;
  return _ZopfliMaxCachedSublen(cache);
}

#endif  /* ZOPFLI_LONGEST_MATCH_CACHE */
