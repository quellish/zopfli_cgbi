/*
 * Optimized assembly code for encodeLZ77.
 * Author: yumeyao(yumeyao#gmail.com)
 * Note:
 *   1. This code assumes the content of *hash is consistent as the content is cached on the stack.
 *   2. This code assumes the value of lazymatching is 1 or 0.
 *   3. This code assumes lodepng_realloc is realloc and realloc is from MSVCRT.lib(call dword ptr [realloc]).
 */
extern "C" __declspec(dllimport) void * __cdecl realloc(void *, size_t);

typedef struct uivector uivector;
typedef struct Hash Hash;
#define MAX_SUPPORTED_DEFLATE_LENGTH 258

#define encodeLZ77(out, hash, in, inpos, insize, windowsize, minmatch, nicematch, lazymatching) \
        _encodeLZ77(inpos, windowsize, out, hash, in, insize, minmatch, nicematch, lazymatching)
__declspec(naked)
unsigned __fastcall _encodeLZ77(size_t inpos, unsigned windowsize,
           /*arg(1) - arg(4)*/  uivector* out, Hash* hash, const unsigned char* in, size_t insize, 
           /*arg(5) - arg(7)*/  unsigned minmatch, unsigned nicematch, unsigned lazymatching)
{
#define stacksize (32+16)
__asm {
	mov eax, edx
	push ebx
	shr eax, 3
	push esi
	xor ebx, ebx
	push edi
	cmp edx, 8192
	push ebp
	lea esi, [ebx+64] /*mov esi, 64*/
	push edx
#define windowsize (shortloopindicator+4)
	cmovae eax, edx
	mov edi, MAX_SUPPORTED_DEFLATE_LENGTH
	push ebx /* if 0, this func is not called by self, else "called" by self (addLengthDistance) indicating short loop1. */
#define shortloopindicator (maxchainlength+4)
	cmovae esi, edi
	push eax
#define maxchainlength (maxlazymatch+4)
	sbb bl, bl
	push esi
#define maxlazymatch (stacksize+beyondstack)
	not bl /* bl = usezeros = windowsize >= 8192 ? 0xFF : 0 */
	       /* ebx = movzx bl, ebx's sign bit = lazy = 0 */
	sub esp, stacksize
#define beyondstack 0
#define varframe (stacksize+beyondstack)
#define funcframe (varframe+16+16)
#define arg(n) [esp+funcframe+n*4]
#define hash_head beyondstack
#define hash_val (beyondstack+4)
#define hash_chain (beyondstack+8)
#define hash_zeros (beyondstack+12)

	/*lea edi, [esp+hash_head]*/ mov edi, esp
	/*mov esi, arg(2)*//*hash*/ mov esi, [edi+funcframe+2*4]
	xchg eax, ecx
	movsd
	movsd
	movsd
	movsd
	mov ebp, arg(3)/*in*/ /*mov ebp, [edi+funcframe+3*4-16]*/
	/*mov esi, arg(4)*//*insize*/ mov esi, [edi+funcframe+4*4-16]
	/*mov bh, arg(7)*//*lazymatching*/ mov bh, [edi+funcframe+7*4-16]
	add esi, ebp
	add ebp, eax
	push esi
#undef beyondstack
#define beyondstack 4
#define maxpos 0
#define hashval (varframe-4)
#define lastptr (varframe-8)
#define length (varframe-12)           /* [chainlength...length] are used as a buffer in loop1addLengthDistance */
#define offset (varframe-16)
#define wpos (varframe-20)
#define chainlength (varframe-24)
#define addLengthDistanceBuffer chainlength
#define lazylength (varframe-28)
#define lazyoffset (varframe-32)
	mov edi, edx
	shr ebx, 7
	cmp ebp, esi
	jae loop1end
	/*
	 loop1:
	 bl - (usezeros ? 1 : 0) | (lazymatching ? 2 : 0)
	 bh - (usezeros && hashval == 0) ? 1 : 0
	 */
loop1:
	xor edx, edx
	div edi
	xor eax, eax
	sub esi, ebp
	jbe getHashend
	cmp esi, 3
	jb getHashdoless
	movzx edi, byte ptr [ebp+2]
	shl edi, 4
	xor eax, edi
getHashdo2:
	movzx edi, byte ptr [ebp+1]
	shl edi, 2
	xor eax, edi
getHashdo1:
	movzx edi, byte ptr [ebp]
	xor eax, edi
	/* currently eax always < HASH_NUM_VALUES=65536 */
	/* movzx eax, ax */
getHashend:
	mov esi, [esp+hash_head]
	mov edi, [esp+hash_val]
	mov ecx, [esi+4*eax]
	mov [edi+4*edx], eax
	mov [esi+4*eax], edx
	mov esi, [esp+hash_chain]
	cmp ecx, -1
	je updateHashChain1skip
	mov [esi+2*edx], cx
updateHashChain1skip:
	test eax, eax
	mov ecx, [esp+maxpos]
	mov [esp+hashval], eax
	setz bh    /* bh = hashval == 0 ? 1 : 0 */
	lea eax, [ebp+MAX_SUPPORTED_DEFLATE_LENGTH]
	cmp ecx, eax
	lea edi, [ebp-1]
	cmova ecx, eax
	and bh, bl /* bh = (usezeros && hashval == 0) ? 1 : 0 */
	mov [esp+lastptr], ecx
	jz loop1skipusezeros
	xor eax, eax
FindZerosNext:
	inc edi
	cmp edi, ecx
	jae FindZerosEnd
	cmp al, [edi]
	jz FindZerosNext
FindZerosEnd:
	mov ecx, [esp+hash_zeros]
	sub edi, ebp
	mov [ecx+2*edx], di
loop1skipusezeros:
	mov eax, [esp+shortloopindicator]
	sub eax, 1
	adc eax, 0
	mov [esp+shortloopindicator], eax
	jnz loop1next

	mov eax, [esp+maxchainlength]
	mov [esp+wpos], edx
	mov [esp+chainlength], eax
	movzx eax, word ptr [esi+2*edx]
	and dword ptr [esp+length], 0
	mov ecx, edx
	and dword ptr [esp+offset], 0

	/*
	 loop2:
	 eax - hashpos
	 ecx - prevpos -> free register(only one, and only ecx can be used as rep counter)
	 edx - wpos -> current_offset
	 esi - foreptr -> current_length
	 edi - numzeros ->(push save) backptr for cmps -> (pop restore) numzeros
	 bh - usezeros && hashval == 0
	 ebp - &in[pos]
	*/
/*loop2start: ecx == edx*/
/*	cmp ecx, edx
	ja L0*/
loop2start:
	cmp edx, eax
	jae L1
loop2continue:
	sub dword ptr [esp+chainlength], 1
	jb loop2end
	sub edx, eax
	ja L2
	jz L4
	add edx, [esp+windowsize]
	jz L4
L2:
	lea esi, [ebp-1]
	test bh, bh
	jz L3
	mov ecx, [esp+hash_val]
	cmp dword ptr [ecx+4*eax], 0
	jnz L3
	mov ecx, [esp+hash_zeros]
	movzx ecx, word ptr [ecx+2*eax]
	cmp ecx, edi
	cmova ecx, edi
	add esi, ecx
L3:
	mov ecx, [esp+lastptr]
	bswap ebx
	neg edx
loop3:
	inc esi
	cmp esi, ecx
	jae loop3end
	mov bh, [esi]
	cmp bh, [esi+edx]
	je loop3
loop3end:
	sub esi, ebp
	bswap ebx
	neg edx
	cmp esi, [esp+length]
	jbe L4
	cmp esi, arg(6)/*nicematch*/
	mov [esp+length], esi
	mov [esp+offset], edx
	jae loop2end
	cmp esi, MAX_SUPPORTED_DEFLATE_LENGTH
	je loop2end
L4:
	mov ecx, [esp+hash_chain]
	movzx ecx, word ptr [ecx+2*eax]
	mov edx, [esp+wpos]
	cmp ecx, eax
	je loop2end
	xchg ecx, eax
/*loop2start: ecx == edx*/
	cmp ecx, edx
	jbe loop2start
L0:
	cmp edx, eax
	jae loop2end
L1:
	cmp eax, ecx
	jbe loop2continue
loop2end:
	mov edx, [esp+length]
	mov edi, [esp+offset]
	mov esi, arg(1)/*out*/
	test bl, 2
	jz loop1skiplazymatching
	test ebx, ebx
	js loop1lazy
	cmp edx, 3
	jb loop1pushback
	cmp edx, [esp+maxlazymatch]
	ja loop1skiplazymatchinglengthae3
	or ebx, 0xFFFF0000 /*lazy = 1*/
	mov [esp+lazylength], edx
	mov [esp+lazyoffset], edi
loop1next:
	inc ebp
	mov esi, [esp+maxpos]
	mov edi, [esp+windowsize]
	mov eax, ebp
	sub eax, arg(3)/*in*/
	cmp ebp, esi
	jb loop1
loop1end:
	xor eax, eax
error_return:
	add esp, funcframe - 16
	pop ebp
	pop edi
	pop esi
	pop ebx
	ret 4*7


loop1lazy:
	mov eax, [esp+lazylength]
	movzx ebx, bx /*lazy = 0*/
	lea ecx, [eax+1]
	/* if(pos == 0) ERROR_BREAK(81); */ /* not impossible */ /*cmp ebp, arg(3)   mov ecx, 81   jz error_return */
	cmp edx, ecx
	ja loop1lazypushback
	xchg eax, edx
	mov ecx, [esp+hash_head]
	mov eax, [esp+hashval]
	mov edi, [esp+lazyoffset]
	dec ebp
	or dword ptr [ecx+4*eax], -1
loop1skiplazymatching:
	cmp edx, 3
	jb loop1pushback
loop1skiplazymatchinglengthae3:
	cmp edi, [esp+windowsize]
	mov eax, 86
	ja error_return
	cmp edx, arg(5)/*minmatch*/
	jb loop1pushback
	cmp edx, 3
	jnz loop1addLengthDistance
	cmp edi, 4096
	ja loop1pushback
loop1addLengthDistance:
	mov [esp+shortloopindicator], edx   /* save length for controlling shortloop1 */
	lea edx, [edx-3]
	xor eax, eax
	cmp edx, 8                /*length-3     index      extra_length*/
	jbe LengthOK              /* 0-8          a/1           a%1     */
	bsr ecx, edx              /* 8-17         a/2+4         a%2     */
	lea ecx, [ecx-2]          /* 16-35        a/4+8         a%4     */
	bts eax, ecx              /* 32-71        a/8+12        a%8     */
	dec eax                   /* 64-143       a/16+16       a%16    */
	and eax, edx              /* 128-255      a/32+20       a%32    */
	shr edx, cl
	lea edx, [edx+4*ecx]
LengthOK:
	dec edi
	lea edx, [edx+257]  /* +FIRST_LENGTH_CODE_INDEX */
	mov [esp+addLengthDistanceBuffer+4], eax
	mov [esp+addLengthDistanceBuffer], edx
	xor eax, eax
	cmp edi, 4
	jbe DistanceOK
	bsr ecx, edi
	dec ecx
	bts eax, ecx
	dec eax
	and eax, edi
	shr edi, cl
	lea edi, [edi+2*ecx]
DistanceOK:
	mov [esp+addLengthDistanceBuffer+12], eax
	mov [esp+addLengthDistanceBuffer+8], edi
	mov edi, [esi+4]/*out->size*/
	lea ecx, [edi*4+16]
	call __uivector_resize
	lea ecx, [edi+4]
	mov [esi+4], ecx
#if 1
	lea esi, [esp+addLengthDistanceBuffer]
	lea edi, [eax+edi*4]
	movsd
	movsd
	movsd
	movsd
#else
	lea edi, [eax+edi*4]
	mov eax, [esp+addLengthDistanceBuffer]
	mov ecx, [esp+addLengthDistanceBuffer+4]
	mov edx, [esp+addLengthDistanceBuffer+8]
	mov esi, [esp+addLengthDistanceBuffer+12]
	mov [edi], eax
	mov [edi+4], ecx
	mov [edi+8], edx
	mov [edi+12], esi
#endif
	jmp loop1next

__uivector_size_inc:
	mov edi, [esi+4]/*out->size*/
	lea ecx, [edi*4+4]
__uivector_resize:
	cmp ecx, [esi+8]
	mov eax, [esi]
	jb uivector_sizeok
	lea edi, [ecx+ecx]
	push edi
	push eax
	call dword ptr [realloc]
	add esp,8
	test eax, eax
	jz error_return_83
	mov [esi+8], edi
	mov [esi], eax
	mov edi, [esi+4]
uivector_sizeok:
	ret

coldcode:
	_emit 'y' /* alignment */
	_emit 'M'
loop1lazypushback:
	call __uivector_size_inc
	movzx ecx, byte ptr [ebp-1]
	mov [eax+edi*4], ecx
	mov edx, [esp+length]
	mov edi, [esp+offset]
	inc dword ptr [esi+4]
	jmp loop1skiplazymatching
loop1pushback:
	call __uivector_size_inc
	movzx ecx, byte ptr [ebp]
	mov [eax+edi*4], ecx
	inc dword ptr [esi+4]
	jmp loop1next
getHashdoless:
	cmp esi, 2
	je getHashdo2
	jmp getHashdo1
error_return_83:
	pop edx
	mov eax, 83
	jmp error_return /* FATAL so never mind call / pop unpairness... */
}
}
