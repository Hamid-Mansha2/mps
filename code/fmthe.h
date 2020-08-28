/* fmthe.h: HEADERS FOR DYLAN-LIKE OBJECT FORMATS
 *
 * $Id$
 * Copyright (c) 2001-2020 Ravenbrook Limited.  See end of file for license.
 * Portions copyright (C) 2002 Global Graphics Software.
 */

#ifndef fmthe_h
#define fmthe_h

#include "mps.h"

/* Formats */
extern mps_res_t EnsureHeaderFormat(mps_fmt_t *, mps_arena_t);
extern mps_res_t EnsureHeaderWeakFormat(mps_fmt_t *, mps_arena_t);
extern mps_res_t HeaderFormatCheck(mps_addr_t addr);
extern mps_res_t HeaderWeakFormatCheck(mps_addr_t addr);

#define headerSIZE (32)
#define headerTypeBits 8
#define realTYPE 0x33
#define realHeader (realTYPE + 0x12345600)
#define padTYPE  0xaa
#define headerType(header) ((header) & (((mps_word_t)1 << headerTypeBits) - 1))
#define headerPadSize(header) ((header) >> headerTypeBits)
#define padHeader(size) ((size << headerTypeBits) | padTYPE)

#endif /* fmthe_h */


/* C. COPYRIGHT AND LICENSE
 *
 * Copyright (C) 2001-2020 Ravenbrook Limited <https://www.ravenbrook.com/>.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the
 *   distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
