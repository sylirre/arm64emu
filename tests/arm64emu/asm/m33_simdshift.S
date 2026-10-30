/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* M33 self-test: AdvSIMD SLI / SRI shift-insert at the shift extremes, for
 * every lane width in both the 64- and 128-bit vector forms and in the
 * scalar D-form.
 *
 * The interesting amount is SRI by the element width itself (SRI .2d #64,
 * .4s #32, .4h #16, .8b #8), which the architecture allows: nothing of the
 * source survives and the destination element is preserved whole. The
 * interpreter used to compute it with a plain C `>>` by the element width,
 * which is undefined -- and on the usual host a shift by 64 of a 64-bit
 * value is a no-op, so SRI .2d #64 copied the SOURCE where it must have
 * kept the destination. Only the .2d lane could reach it: an 8/16/32-bit
 * lane still shifts inside a u64, so the narrower widths were right by
 * accident. SLI's mirror extreme (#0, result = Vn) is covered beside it.
 * The 64-bit-form rows also assert that bits 127:64 of the destination are
 * zeroed, which a shift-INSERT operation must still do.
 *
 * x0=0 on success, else failing-check id. Builds with -DUSERMODE as a Linux
 * user binary (exit code = x0) so every expected value is validated under
 * qemu-aarch64 -cpu max first. */

.macro CKX reg, val, id
    movz    x11, #\id
    ldr     x9, =\val
    cmp     \reg, x9
    b.ne    fail
.endm

.text
.global _start
_start:
    ldr     x20, =vdat

    /* sri v0.2d, v1.2d, #64   the whole destination survives */
    ldr     q1, [x20, #0]
    ldr     q0, [x20, #16]
    sri     v0.2d, v1.2d, #64
    umov    x0, v0.d[0]
    CKX     x0, 0x0123456789abcde0, 1
    umov    x0, v0.d[1]
    CKX     x0, 0xdeadbeefcafebab0, 2

    /* sri v0.2d, v1.2d, #63 */
    ldr     q1, [x20, #0]
    ldr     q0, [x20, #16]
    sri     v0.2d, v1.2d, #63
    umov    x0, v0.d[0]
    CKX     x0, 0x0123456789abcde1, 3
    umov    x0, v0.d[1]
    CKX     x0, 0xdeadbeefcafebab1, 4

    /* sri v0.2d, v1.2d, #1 */
    ldr     q1, [x20, #0]
    ldr     q0, [x20, #16]
    sri     v0.2d, v1.2d, #1
    umov    x0, v0.d[0]
    CKX     x0, 0x7f6e5d4c3b2a1908, 5
    umov    x0, v0.d[1]
    CKX     x0, 0xc000000000000000, 6

    /* sri v0.4s, v1.4s, #32   the whole destination survives */
    ldr     q1, [x20, #0]
    ldr     q0, [x20, #16]
    sri     v0.4s, v1.4s, #32
    umov    x0, v0.d[0]
    CKX     x0, 0x0123456789abcde0, 7
    umov    x0, v0.d[1]
    CKX     x0, 0xdeadbeefcafebab0, 8

    /* sri v0.4s, v1.4s, #1 */
    ldr     q1, [x20, #0]
    ldr     q0, [x20, #16]
    sri     v0.4s, v1.4s, #1
    umov    x0, v0.d[0]
    CKX     x0, 0x7f6e5d4cbb2a1908, 9
    umov    x0, v0.d[1]
    CKX     x0, 0xc000000080000000, 10

    /* sri v0.8h, v1.8h, #16   the whole destination survives */
    ldr     q1, [x20, #0]
    ldr     q0, [x20, #16]
    sri     v0.8h, v1.8h, #16
    umov    x0, v0.d[0]
    CKX     x0, 0x0123456789abcde0, 11
    umov    x0, v0.d[1]
    CKX     x0, 0xdeadbeefcafebab0, 12

    /* sri v0.8h, v1.8h, #1 */
    ldr     q1, [x20, #0]
    ldr     q0, [x20, #16]
    sri     v0.8h, v1.8h, #1
    umov    x0, v0.d[0]
    CKX     x0, 0x7f6e5d4cbb2a9908, 13
    umov    x0, v0.d[1]
    CKX     x0, 0xc000800080008000, 14

    /* sri v0.16b, v1.16b, #8   the whole destination survives */
    ldr     q1, [x20, #0]
    ldr     q0, [x20, #16]
    sri     v0.16b, v1.16b, #8
    umov    x0, v0.d[0]
    CKX     x0, 0x0123456789abcde0, 15
    umov    x0, v0.d[1]
    CKX     x0, 0xdeadbeefcafebab0, 16

    /* sri v0.16b, v1.16b, #1 */
    ldr     q1, [x20, #0]
    ldr     q0, [x20, #16]
    sri     v0.16b, v1.16b, #1
    umov    x0, v0.d[0]
    CKX     x0, 0x7f6e5d4cbbaa9988, 17
    umov    x0, v0.d[1]
    CKX     x0, 0xc080808080808080, 18

    /* sri v0.2s, v1.2s, #32   64-bit form: bits 127:64 are zeroed */
    ldr     q1, [x20, #0]
    ldr     q0, [x20, #16]
    sri     v0.2s, v1.2s, #32
    umov    x0, v0.d[0]
    CKX     x0, 0x0123456789abcde0, 19
    umov    x0, v0.d[1]
    CKX     x0, 0x0000000000000000, 20

    /* sri v0.4h, v1.4h, #16 */
    ldr     q1, [x20, #0]
    ldr     q0, [x20, #16]
    sri     v0.4h, v1.4h, #16
    umov    x0, v0.d[0]
    CKX     x0, 0x0123456789abcde0, 21
    umov    x0, v0.d[1]
    CKX     x0, 0x0000000000000000, 22

    /* sri v0.8b, v1.8b, #8 */
    ldr     q1, [x20, #0]
    ldr     q0, [x20, #16]
    sri     v0.8b, v1.8b, #8
    umov    x0, v0.d[0]
    CKX     x0, 0x0123456789abcde0, 23
    umov    x0, v0.d[1]
    CKX     x0, 0x0000000000000000, 24

    /* sri v0.8b, v1.8b, #1 */
    ldr     q1, [x20, #0]
    ldr     q0, [x20, #16]
    sri     v0.8b, v1.8b, #1
    umov    x0, v0.d[0]
    CKX     x0, 0x7f6e5d4cbbaa9988, 25
    umov    x0, v0.d[1]
    CKX     x0, 0x0000000000000000, 26

    /* sli v0.2d, v1.2d, #0   the whole source survives */
    ldr     q1, [x20, #0]
    ldr     q0, [x20, #16]
    sli     v0.2d, v1.2d, #0
    umov    x0, v0.d[0]
    CKX     x0, 0xfedcba9876543210, 27
    umov    x0, v0.d[1]
    CKX     x0, 0x8000000000000001, 28

    /* sli v0.2d, v1.2d, #63 */
    ldr     q1, [x20, #0]
    ldr     q0, [x20, #16]
    sli     v0.2d, v1.2d, #63
    umov    x0, v0.d[0]
    CKX     x0, 0x0123456789abcde0, 29
    umov    x0, v0.d[1]
    CKX     x0, 0xdeadbeefcafebab0, 30

    /* sli v0.4s, v1.4s, #0   the whole source survives */
    ldr     q1, [x20, #0]
    ldr     q0, [x20, #16]
    sli     v0.4s, v1.4s, #0
    umov    x0, v0.d[0]
    CKX     x0, 0xfedcba9876543210, 31
    umov    x0, v0.d[1]
    CKX     x0, 0x8000000000000001, 32

    /* sli v0.4s, v1.4s, #31 */
    ldr     q1, [x20, #0]
    ldr     q0, [x20, #16]
    sli     v0.4s, v1.4s, #31
    umov    x0, v0.d[0]
    CKX     x0, 0x0123456709abcde0, 33
    umov    x0, v0.d[1]
    CKX     x0, 0x5eadbeefcafebab0, 34

    /* sli v0.8h, v1.8h, #15 */
    ldr     q1, [x20, #0]
    ldr     q0, [x20, #16]
    sli     v0.8h, v1.8h, #15
    umov    x0, v0.d[0]
    CKX     x0, 0x0123456709ab4de0, 35
    umov    x0, v0.d[1]
    CKX     x0, 0x5ead3eef4afebab0, 36

    /* sli v0.8h, v1.8h, #0 */
    ldr     q1, [x20, #0]
    ldr     q0, [x20, #16]
    sli     v0.8h, v1.8h, #0
    umov    x0, v0.d[0]
    CKX     x0, 0xfedcba9876543210, 37
    umov    x0, v0.d[1]
    CKX     x0, 0x8000000000000001, 38

    /* sli v0.16b, v1.16b, #7 */
    ldr     q1, [x20, #0]
    ldr     q0, [x20, #16]
    sli     v0.16b, v1.16b, #7
    umov    x0, v0.d[0]
    CKX     x0, 0x01234567092b4d60, 39
    umov    x0, v0.d[1]
    CKX     x0, 0x5e2d3e6f4a7e3ab0, 40

    /* sli v0.16b, v1.16b, #0 */
    ldr     q1, [x20, #0]
    ldr     q0, [x20, #16]
    sli     v0.16b, v1.16b, #0
    umov    x0, v0.d[0]
    CKX     x0, 0xfedcba9876543210, 41
    umov    x0, v0.d[1]
    CKX     x0, 0x8000000000000001, 42

    /* sli v0.2s, v1.2s, #31   64-bit form: bits 127:64 are zeroed */
    ldr     q1, [x20, #0]
    ldr     q0, [x20, #16]
    sli     v0.2s, v1.2s, #31
    umov    x0, v0.d[0]
    CKX     x0, 0x0123456709abcde0, 43
    umov    x0, v0.d[1]
    CKX     x0, 0x0000000000000000, 44

    /* sli v0.8b, v1.8b, #0 */
    ldr     q1, [x20, #0]
    ldr     q0, [x20, #16]
    sli     v0.8b, v1.8b, #0
    umov    x0, v0.d[0]
    CKX     x0, 0xfedcba9876543210, 45
    umov    x0, v0.d[1]
    CKX     x0, 0x0000000000000000, 46

    /* ---- scalar (D-form) shift-insert: only esize=64 exists ---- */

    /* sri d0, d1, #64   the whole destination survives */
    ldr     q1, [x20, #0]
    ldr     q0, [x20, #16]
    sri     d0, d1, #64
    umov    x0, v0.d[0]
    CKX     x0, 0x0123456789abcde0, 47
    umov    x0, v0.d[1]
    CKX     x0, 0x0000000000000000, 48

    /* sri d0, d1, #63 */
    ldr     q1, [x20, #0]
    ldr     q0, [x20, #16]
    sri     d0, d1, #63
    umov    x0, v0.d[0]
    CKX     x0, 0x0123456789abcde1, 49
    umov    x0, v0.d[1]
    CKX     x0, 0x0000000000000000, 50

    /* sri d0, d1, #1 */
    ldr     q1, [x20, #0]
    ldr     q0, [x20, #16]
    sri     d0, d1, #1
    umov    x0, v0.d[0]
    CKX     x0, 0x7f6e5d4c3b2a1908, 51
    umov    x0, v0.d[1]
    CKX     x0, 0x0000000000000000, 52

    /* sli d0, d1, #0   the whole source survives */
    ldr     q1, [x20, #0]
    ldr     q0, [x20, #16]
    sli     d0, d1, #0
    umov    x0, v0.d[0]
    CKX     x0, 0xfedcba9876543210, 53
    umov    x0, v0.d[1]
    CKX     x0, 0x0000000000000000, 54

    /* sli d0, d1, #63 */
    ldr     q1, [x20, #0]
    ldr     q0, [x20, #16]
    sli     d0, d1, #63
    umov    x0, v0.d[0]
    CKX     x0, 0x0123456789abcde0, 55
    umov    x0, v0.d[1]
    CKX     x0, 0x0000000000000000, 56

    mov     x0, #0
done:
#ifdef USERMODE
    movz    x8, #93                      /* exit(x0) */
    svc     #0
#else
    hlt     #0
#endif
fail:
    mov     x0, x11
    b       done

.data
.balign 16
vdat:
    .quad 0xfedcba9876543210, 0x8000000000000001   /* +0:  Vn */
    .quad 0x0123456789abcde0, 0xdeadbeefcafebab0   /* +16: Vd */
