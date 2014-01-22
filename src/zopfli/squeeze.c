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

#include "squeeze.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "ftol_magic.h"
#include "blocksplitter.h"
#include "deflate.h"
#include "tree.h"
#include "util.h"

typedef struct SymbolStats {
  /* The literal and length symbols. */
  size_t litlens[288];
  /* The 32 unique dist symbols, not the 32768 possible dists. */
  size_t dists[32];

  double ll_symbols[288];  /* Length of each lit/len symbol in bits. */
  double d_symbols[32];  /* Length of each dist symbol in bits. */
} SymbolStats;

/* Sets everything to 0. */
static void InitStats(SymbolStats* stats) {
  memset(stats, 0, sizeof(*stats));
}

static void CopyStats(SymbolStats* source, SymbolStats* dest) {
  memcpy(dest, source, sizeof(*dest));
}

/* Adds the bit lengths. */
static void __AddWeighedStatFreqs(SymbolStats* stats1,
                                  const SymbolStats* stats2) {
  int i;
  size_t *src1 = stats1->litlens;
  const size_t *src2 = stats2->litlens;
  for (i = 0; i < (288 + 32) /* litlens & dists */ ; i++) {
    src1[i] = (size_t) (src1[i] + src2[i] / 2);
  }
  src1[256] = 1;  /* End symbol. */
}

typedef struct RanState {
  unsigned int m_w, m_z;
} RanState;

static void InitRanState(RanState* state) {
  state->m_w = 1;
  state->m_z = 2;
}

/* Get random number: "Multiply-With-Carry" generator of G. Marsaglia */
static unsigned int Ran(RanState* state) {
  state->m_z = 36969 * (state->m_z & 65535) + (state->m_z >> 16);
  state->m_w = 18000 * (state->m_w & 65535) + (state->m_w >> 16);
  return (state->m_z << 16) + state->m_w;  /* 32-bit result. */
}

static void RandomizeFreqs(RanState* state, size_t* freqs, int n) {
  int i;
  for (i = 0; i < n; i++) {
    if ((Ran(state) >> 4) % 3 == 0) freqs[i] = freqs[Ran(state) % n];
  }
}

static void RandomizeStatFreqs(RanState* state, SymbolStats* stats) {
  RandomizeFreqs(state, stats->litlens, 288+32);
  stats->litlens[256] = 1;  /* End symbol. */
}

static void ClearStatFreqs(SymbolStats* stats) {
  size_t i;
  for (i = 0; i < 288+32; i++) stats->litlens[i] = 0;
}

#if defined(_MSC_VER)
#define ZOPFLI_SQUEEZE_FASTCALL __fastcall
#else
#define ZOPFLI_SQUEEZE_FASTCALL
#endif

/*
Function that calculates a cost based on a model for the given LZ77 symbol.
litlen: means literal symbol if dist is 0, length otherwise.
*/
typedef double ZOPFLI_SQUEEZE_FASTCALL
CostModelFun(unsigned litlen, unsigned dist, void* context);

/*
Cost model which should exactly match fixed tree.
type: CostModelFun
*/
#if defined(_MSC_VER) && !defined(DEBUG) && !defined(_DEBUG)
__declspec(naked) static double ZOPFLI_SQUEEZE_FASTCALL
GetCostFixed(unsigned litlen, unsigned dist, void* unused) {
  extern const unsigned short ZopfliGetLengthSymbolTable[];
  extern const unsigned char ZopfliGetLengthExtraBitsTable[];
__asm {
	test edx, edx
	jnz distnotzero
	cmp ecx, 144
	sbb edx, -9 /* edx = 0 + 9 - litlen<144?1:0 */
	mov [esp+4], edx
	fild dword ptr [esp+4]
	ret 4
	/* alignment */
	__emit 'y' __asm __emit 'u' __asm __emit 'M' __asm __emit 'e'
	__asm __emit 'Y' __asm __emit 'a' __asm __emit 'o' __asm nop
distnotzero:
	cmp edx, 5
	mov eax, 3
	cmovb edx, eax /* so that bsr makes result = 1 */
	cmp word ptr ZopfliGetLengthSymbolTable[2*ecx], 280
	movzx eax, byte ptr ZopfliGetLengthExtraBitsTable[ecx]
	sbb eax, -(7+5)
	dec edx
	bsr edx, edx
	/* eax = lbits + costs - 1; edx = dbits + 1; */
	add eax, edx
	mov [esp+4], eax
	fild dword ptr [esp+4]
	ret 4
}
}
#else
static double ZOPFLI_SQUEEZE_FASTCALL
GetCostFixed(unsigned litlen, unsigned dist, void* unused) {
  int ret;
  (void)unused;
  if (dist == 0) {
    if (litlen < 144) ret = 8;
    else ret = 9;
  } else {
    int dbits = ZopfliGetDistExtraBits(dist);
    int lbits = ZopfliGetLengthExtraBits(litlen);
    int lsym = ZopfliGetLengthSymbol(litlen);
    int cost;
    if (lsym < 280) cost = 7 + 5;
	else cost = 8 + 5;
    /* Every dist symbol has length 5. */
    ret = cost + dbits + lbits;
  }
  return ret;
}
#endif

/*
Cost model based on symbol statistics.
type: CostModelFun
*/
static double ZOPFLI_SQUEEZE_FASTCALL
GetCostStat(unsigned litlen, unsigned dist, void* context) {
  SymbolStats* stats = (SymbolStats*)context;
  if (dist == 0) {
    return stats->ll_symbols[litlen];
  } else {
    int lsym = ZopfliGetLengthSymbol(litlen);
    int lbits = ZopfliGetLengthExtraBits(litlen);
    int dsym = ZopfliGetDistSymbol(dist);
    int dbits = ZopfliGetDistExtraBits(dist);
    /*return stats->ll_symbols[lsym] + lbits + stats->d_symbols[dsym] + dbits;*/
	return stats->ll_symbols[lsym] + stats->d_symbols[dsym] + (lbits + dbits);
  }
}

/*
Finds the minimum possible cost this cost model can return for valid length and
distance symbols.
*/
static double GetCostModelMinCost(CostModelFun* costmodel, void* costcontext) {
  double mincost;
  int bestlength = 0; /* length that has lowest cost in the cost model */
  int bestdist = 0; /* distance that has lowest cost in the cost model */
  int i;

  mincost = ZOPFLI_LARGE_FLOAT;
  for (i = 3; i < 259; i++) {
    double c = costmodel(i, 1, costcontext);
    if (c < mincost) {
      bestlength = i;
      mincost = c;
    }
  }

  mincost = ZOPFLI_LARGE_FLOAT;
  for (i = 0; i < 30; i++) {
    double c = costmodel(3, DistSymbols[i], costcontext);
    if (c < mincost) {
      bestdist = DistSymbols[i];
      mincost = c;
    }
  }

  return costmodel(bestlength, bestdist, costcontext);
}

/*
Performs the forward pass for "squeeze". Gets the most optimal length to reach
every byte from a previous byte, using cost calculations.
s: the ZopfliBlockState
in: the input data array
instart: where to start
inend: where to stop (not inclusive)
costmodel: function to calculate the cost of some lit/len/dist pair.
costcontext: abstract context for the costmodel function
length_array: output array of size (inend - instart) which will receive the best
    length to reach this byte from a previous byte.
returns the cost that was, according to the costmodel, needed to get to the end.
*/
static double GetBestLengths(ZopfliBlockState *s,
                             const unsigned char* in,
                             size_t instart, size_t inend,
                             CostModelFun* costmodel, void* costcontext,
                             unsigned short* length_array) {
  /* Best cost to get here so far. */
  size_t blocksize = inend - instart;
  float* costs;
  size_t i = 0, k;
  unsigned short leng;
  unsigned short dist;
  unsigned short sublen[259];
  size_t windowstart = instart > ZOPFLI_WINDOW_SIZE
      ? instart - ZOPFLI_WINDOW_SIZE : 0;
  ZopfliHash hash;
  ZopfliHash* h = &hash;
  double result;
  double mincost = GetCostModelMinCost(costmodel, costcontext);

  if (instart == inend) return 0;

  costs = (float*)malloc(sizeof(float) * (blocksize + 1));
  if (!costs) exit(-1); /* Allocation failed. */

  ZopfliInitHash(ZOPFLI_WINDOW_SIZE, h);
  ZopfliWarmupHash(in, windowstart, inend, h);
  for (i = windowstart; i < instart; i++) {
    ZopfliUpdateHash(in, i, inend, h);
  }

  for (i = 1; i < blocksize + 1; i++) costs[i] = ZOPFLI_LARGE_FLOAT;
  costs[0] = 0;  /* Because it's the start. */
  length_array[0] = 0;

  for (i = instart; i < inend; i++) {
    size_t j = i - instart;  /* Index in the costs array and length_array. */
    ZopfliUpdateHash(in, i, inend, h);

#ifdef ZOPFLI_SHORTCUT_LONG_REPETITIONS
    /* If we're in a long repetition of the same character and have more than
    ZOPFLI_MAX_MATCH characters before and after our position. */
    if (h->same[i & ZOPFLI_WINDOW_MASK] > ZOPFLI_MAX_MATCH * 2
        && i > instart + ZOPFLI_MAX_MATCH + 1
        && i + ZOPFLI_MAX_MATCH * 2 + 1 < inend
        && h->same[(i - ZOPFLI_MAX_MATCH) & ZOPFLI_WINDOW_MASK]
            > ZOPFLI_MAX_MATCH) {
      double symbolcost = costmodel(ZOPFLI_MAX_MATCH, 1, costcontext);
      /* Set the length to reach each one to ZOPFLI_MAX_MATCH, and the cost to
      the cost corresponding to that length. Doing this, we skip
      ZOPFLI_MAX_MATCH values to avoid calling ZopfliFindLongestMatch. */
      for (k = 0; k < ZOPFLI_MAX_MATCH; k++) {
        costs[j + ZOPFLI_MAX_MATCH] = costs[j] + symbolcost;
        length_array[j + ZOPFLI_MAX_MATCH] = ZOPFLI_MAX_MATCH;
        i++;
        j++;
        ZopfliUpdateHash(in, i, inend, h);
      }
    }
#endif

    ZopfliFindLongestMatch(s, h, in, i, inend, ZOPFLI_MAX_MATCH, sublen,
                           &dist, &leng);

    /* Literal. */
    if (i + 1 <= inend) {
      double newCost = costs[j] + costmodel(in[i], 0, costcontext);
      assert(newCost >= 0);
      if (newCost < costs[j + 1]) {
        costs[j + 1] = newCost;
        length_array[j + 1] = 1;
      }
    }
    /* Lengths. */
    for (k = 3; k <= leng && i + k <= inend; k++) {
      double newCost;

      /* Calling the cost model is expensive, avoid this if we are already at
      the minimum possible cost that it can return. */
     if (costs[j + k] - costs[j] <= mincost) continue;

      newCost = costs[j] + costmodel(k, sublen[k], costcontext);
      assert(newCost >= 0);
      if (newCost < costs[j + k]) {
        assert(k <= ZOPFLI_MAX_MATCH);
        costs[j + k] = newCost;
        length_array[j + k] = k;
      }
    }
  }

  assert(costs[blocksize] >= 0);
  result = costs[blocksize];

  ZopfliCleanHash(h);
  free(costs);

  return result;
}

/*
Calculates the optimal path of lz77 lengths to use, from the calculated
length_array. The length_array must contain the optimal length to reach that
byte. The path will be filled with the lengths to use, so its data size will be
the amount of lz77 symbols.
*/
static void TraceBackwards(size_t size, const unsigned short* length_array,
                           unsigned short** path, size_t* pathsize) {
  size_t index = size;
  unsigned short *_path;
  size_t _pathsize;
  if (size == 0) return;
  _path = *path;
  _pathsize = *pathsize;
  for (;;) {
    ZOPFLI_APPEND_DATA_T(unsigned short, length_array[index], _path, _pathsize);
    assert(length_array[index] <= index);
    assert(length_array[index] <= ZOPFLI_MAX_MATCH);
    assert(length_array[index] != 0);
    index -= length_array[index];
    if (index == 0) break;
  }
  *path = _path;
  *pathsize = _pathsize;

  /* Mirror result. */
  for (index = 0; index < _pathsize / 2; index++) {
    unsigned short temp = _path[index];
    _path[index] = _path[_pathsize - index - 1];
    _path[_pathsize - index - 1] = temp;
  }
}

static void FollowPath(ZopfliBlockState* s,
                       const unsigned char* in, size_t instart, size_t inend,
                       unsigned short* path, size_t pathsize,
                       ZopfliLZ77Store* store) {
  size_t i, j, pos = 0;
  size_t windowstart = instart > ZOPFLI_WINDOW_SIZE
      ? instart - ZOPFLI_WINDOW_SIZE : 0;

  size_t total_length_test = 0;

  ZopfliHash hash;
  ZopfliHash* h = &hash;

  if (instart == inend) return;

  ZopfliInitHash(ZOPFLI_WINDOW_SIZE, h);
  ZopfliWarmupHash(in, windowstart, inend, h);
  for (i = windowstart; i < instart; i++) {
    ZopfliUpdateHash(in, i, inend, h);
  }

  pos = instart;
  for (i = 0; i < pathsize; i++) {
    unsigned short length = path[i];
    unsigned short dummy_length;
    unsigned short dist;
    assert(pos < inend);

    ZopfliUpdateHash(in, pos, inend, h);

    /* Add to output. */
    if (length >= ZOPFLI_MIN_MATCH) {
      /* Get the distance by recalculating longest match. The found length
      should match the length from the path. */
      ZopfliFindLongestMatch(s, h, in, pos, inend, length, 0,
                             &dist, &dummy_length);
      assert(!(dummy_length != length && length > 2 && dummy_length > 2));
      ZopfliVerifyLenDist(in, inend, pos, dist, length);
      ZopfliStoreLitLenDist(length, dist, store);
      total_length_test += length;
    } else {
      length = 1;
      ZopfliStoreLitLenDist(in[pos], 0, store);
      total_length_test++;
    }


    assert(pos + length <= inend);
    for (j = 1; j < length; j++) {
      ZopfliUpdateHash(in, pos + j, inend, h);
    }

    pos += length;
  }

  ZopfliCleanHash(h);
}

/* Calculates the entropy of the statistics */
static void CalculateStatistics(SymbolStats* stats) {
  ZopfliCalculateEntropy(stats->litlens, 288, stats->ll_symbols);
  ZopfliCalculateEntropy(stats->dists, 32, stats->d_symbols);
}

/* Appends the symbol statistics from the store. */
static void GetStatistics(const ZopfliLZ77Store* store, SymbolStats* stats) {
  size_t i;
  for (i = 0; i < store->size; i++) {
    if (store->dists[i] == 0) {
      stats->litlens[store->litlens[i]]++;
    } else {
      stats->litlens[ZopfliGetLengthSymbol(store->litlens[i])]++;
      stats->dists[ZopfliGetDistSymbol(store->dists[i])]++;
    }
  }
  stats->litlens[256] = 1;  /* End symbol. */

  CalculateStatistics(stats);
}

/*
Does a single run for ZopfliLZ77Optimal. For good compression, repeated runs
with updated statistics should be performed.

s: the block state
in: the input data array
instart: where to start
inend: where to stop (not inclusive)
path: pointer to dynamically allocated memory to store the path
pathsize: pointer to the size of the dynamic path array
length_array: array if size (inend - instart) used to store lengths
costmodel: function to use as the cost model for this squeeze run
costcontext: abstract context for the costmodel function
store: place to output the LZ77 data
returns the cost that was, according to the costmodel, needed to get to the end.
    This is not the actual cost.
*/
static double LZ77OptimalRun(ZopfliBlockState* s,
    const unsigned char* in, size_t instart, size_t inend,
    unsigned short** path, size_t* pathsize,
    unsigned short* length_array, CostModelFun* costmodel,
    void* costcontext, ZopfliLZ77Store* store) {
  double cost = GetBestLengths(
      s, in, instart, inend, costmodel, costcontext, length_array);
  free(*path);
  *path = 0;
  *pathsize = 0;
  TraceBackwards(inend - instart, length_array, path, pathsize);
  FollowPath(s, in, instart, inend, *path, *pathsize, store);
  assert(cost < ZOPFLI_LARGE_FLOAT);
  return cost;
}

void ZopfliLZ77Optimal(ZopfliBlockState *s,
                       const unsigned char* in, size_t instart, size_t inend,
                       ZopfliLZ77Store* store) {
  /* Dist to get to here with smallest cost. */
  size_t blocksize = inend - instart;
  unsigned short* length_array =
      (unsigned short*)malloc(sizeof(unsigned short) * (blocksize + 1));
  unsigned short* path = 0;
  size_t pathsize = 0;
  ZopfliLZ77Store currentstore;
  SymbolStats stats, beststats, laststats;
  int i;
  size_t cost;
  size_t bestcost = -1;
  size_t lastcost = 0;
  /* Try randomizing the costs a bit once the size stabilizes. */
  RanState ran_state;
  int lastrandomstep = -1;

  if (!length_array) exit(-1); /* Allocation failed. */

  InitRanState(&ran_state);
  InitStats(&stats);
  ZopfliInitLZ77Store(&currentstore);

  /* Do regular deflate, then loop multiple shortest path runs, each time using
  the statistics of the previous run. */

  /* Initial run. */
  ZopfliLZ77Greedy(s, in, instart, inend, &currentstore);
  GetStatistics(&currentstore, &stats);

  /* Repeat statistics with each time the cost model from the previous stat
  run. */
  for (i = 0; i < s->options->numiterations; i++) {
    ZopfliCleanLZ77Store(&currentstore);
    ZopfliInitLZ77Store(&currentstore);
    LZ77OptimalRun(s, in, instart, inend, &path, &pathsize,
                   length_array, GetCostStat, (void*)&stats,
                   &currentstore);
    cost = ZopfliCalculateBlockSize(currentstore.litlens, currentstore.dists,
                                    0, currentstore.size, 2);
    if (s->options->verbose_more || (s->options->verbose && cost < bestcost)) {
      ftol_magic cost2;
	  ftol_magic_setdouble(cost2, cost);
	  ftol_magic_convert(cost2);
      fprintf(stderr, "Iteration %d: %d bit\n", i, ftol_magic_getlong(cost2));
    }
    if (cost < bestcost) {
      /* Copy to the output store. */
      ZopfliCopyLZ77Store(&currentstore, store);
      CopyStats(&stats, &beststats);
      bestcost = cost;
    }
    CopyStats(&stats, &laststats);
    ClearStatFreqs(&stats);
    GetStatistics(&currentstore, &stats);
    if (lastrandomstep != -1) {
      /* This makes it converge slower but better. Do it only once the
      randomness kicks in so that if the user does few iterations, it gives a
      better result sooner. */
      __AddWeighedStatFreqs(&stats, &laststats);
      CalculateStatistics(&stats);
    }
    if (i > 5 && cost == lastcost) {
      CopyStats(&beststats, &stats);
      RandomizeStatFreqs(&ran_state, &stats);
      CalculateStatistics(&stats);
      lastrandomstep = i;
    }
    lastcost = cost;
  }

  free(length_array);
  free(path);
  ZopfliCleanLZ77Store(&currentstore);
}

void ZopfliLZ77OptimalFixed(ZopfliBlockState *s,
                            const unsigned char* in,
                            size_t instart, size_t inend,
                            ZopfliLZ77Store* store)
{
  /* Dist to get to here with smallest cost. */
  size_t blocksize = inend - instart;
  unsigned short* length_array =
      (unsigned short*)malloc(sizeof(unsigned short) * (blocksize + 1));
  unsigned short* path = 0;
  size_t pathsize = 0;

  if (!length_array) exit(-1); /* Allocation failed. */

  s->blockstart = instart;
  s->blockend = inend;

  /* Shortest path for fixed tree This one should give the shortest possible
  result for fixed tree, no repeated runs are needed since the tree is known. */
  LZ77OptimalRun(s, in, instart, inend, &path, &pathsize,
                 length_array, GetCostFixed, 0, store);

  free(length_array);
  free(path);
}
