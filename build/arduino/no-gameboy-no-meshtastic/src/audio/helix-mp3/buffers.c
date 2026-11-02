/* ***** BEGIN LICENSE BLOCK ***** 
 * Version: RCSL 1.0/RPSL 1.0 
 *  
 * Portions Copyright (c) 1995-2002 RealNetworks, Inc. All Rights Reserved. 
 *      
 * The contents of this file, and the files included with this file, are 
 * subject to the current version of the RealNetworks Public Source License 
 * Version 1.0 (the "RPSL") available at 
 * http://www.helixcommunity.org/content/rpsl unless you have licensed 
 * the file under the RealNetworks Community Source License Version 1.0 
 * (the "RCSL") available at http://www.helixcommunity.org/content/rcsl, 
 * in which case the RCSL will apply. You may also obtain the license terms 
 * directly from RealNetworks.  You may not use this file except in 
 * compliance with the RPSL or, if you have a valid RCSL with RealNetworks 
 * applicable to this file, the RCSL.  Please see the applicable RPSL or 
 * RCSL for the rights, obligations and limitations governing use of the 
 * contents of the file.  
 *  
 * This file is part of the Helix DNA Technology. RealNetworks is the 
 * developer of the Original Code and owns the copyrights in the portions 
 * it created. 
 *  
 * This file, and the files included with this file, is distributed and made 
 * available on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER 
 * EXPRESS OR IMPLIED, AND REALNETWORKS HEREBY DISCLAIMS ALL SUCH WARRANTIES, 
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY, FITNESS 
 * FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT. 
 * 
 * Technology Compatibility Kit Test Suite(s) Location: 
 *    http://www.helixcommunity.org/content/tck 
 * 
 * Contributor(s): 
 *  
 * ***** END LICENSE BLOCK ***** */ 

/**************************************************************************************
 * Fixed-point MP3 decoder
 * Jon Recker (jrecker@real.com), Ken Cooke (kenc@real.com)
 * June 2003
 *
 * buffers.c - allocation and freeing of internal MP3 decoder buffers
 *
 * All memory allocation for the codec is done in this file, so if you don't want 
 *  to use other the  helix_malloc() and helix_free() for heap management this is 
 *  the only file you'll need to change.
 **************************************************************************************/

//#include "hlxclib/stdlib.h"		/* for helix_malloc, free */ 
#include <stdlib.h>
#include <string.h>
#include "coder.h"
#include "utils/helix_memory.h"

/**************************************************************************************
 * Function:    ClearBuffer
 *
 * Description: fill buffer with 0's
 *
 * Inputs:      pointer to buffer
 *              number of bytes to fill with 0
 *
 * Outputs:     cleared buffer
 *
 * Return:      none
 *
 * Notes:       slow, platform-independent equivalent to memset(buf, 0, nBytes)
 **************************************************************************************/
static void ClearBuffer(void *buf, int nBytes)
{
	int i;
	unsigned char *cbuf = (unsigned char *)buf;

	for (i = 0; i < nBytes; i++)
		cbuf[i] = 0;

	return;
}

/**************************************************************************************
 * Function:    AllocateBuffers
 *
 * Description: allocate all the memory needed for the MP3 decoder
 *
 * Inputs:      none
 *
 * Outputs:     none
 *
 * Return:      pointer to MP3DecInfo structure (initialized with pointers to all 
 *                the internal buffers needed for decoding, all other members of 
 *                MP3DecInfo structure set to 0)
 *
 * Notes:       if one or more helix_mallocs fail, function frees any buffers already
 *                allocated before returning
 **************************************************************************************/
MP3DecInfo *AllocateBuffers(void)
{
	MP3DecInfo *mp3DecInfo;
	FrameHeader *fh;
	SideInfo *si;
	ScaleFactorInfo *sfi;
	HuffmanInfo *hi;
	DequantInfo *di;
	IMDCTInfo *mi;
	SubbandInfo *sbi;

	mp3DecInfo = (MP3DecInfo *)helix_malloc(sizeof(MP3DecInfo));
	if (!mp3DecInfo)
		return 0;
	ClearBuffer(mp3DecInfo, sizeof(MP3DecInfo));
	
	fh =  (FrameHeader *)     helix_malloc(sizeof(FrameHeader));
	si =  (SideInfo *)        helix_malloc(sizeof(SideInfo));
	sfi = (ScaleFactorInfo *) helix_malloc(sizeof(ScaleFactorInfo));
	hi =  (HuffmanInfo *)     helix_malloc(sizeof(HuffmanInfo));
	di =  (DequantInfo *)     helix_malloc(sizeof(DequantInfo));
	mi =  (IMDCTInfo *)       helix_malloc(sizeof(IMDCTInfo));
	sbi = (SubbandInfo *)     helix_malloc(sizeof(SubbandInfo));

	mp3DecInfo->FrameHeaderPS =     (void *)fh;
	mp3DecInfo->SideInfoPS =        (void *)si;
	mp3DecInfo->ScaleFactorInfoPS = (void *)sfi;
	mp3DecInfo->HuffmanInfoPS =     (void *)hi;
	mp3DecInfo->DequantInfoPS =     (void *)di;
	mp3DecInfo->IMDCTInfoPS =       (void *)mi;
	mp3DecInfo->SubbandInfoPS =     (void *)sbi;

	if (!fh || !si || !sfi || !hi || !di || !mi || !sbi) {
		FreeBuffers(mp3DecInfo);	/* safe to call - only frees memory that was successfully allocated */
		return 0;
	}

	/* important to do this - DSP primitives assume a bunch of state variables are 0 on first use */
	ClearBuffer(fh,  sizeof(FrameHeader));
	ClearBuffer(si,  sizeof(SideInfo));
	ClearBuffer(sfi, sizeof(ScaleFactorInfo));
	ClearBuffer(hi,  sizeof(HuffmanInfo));
	ClearBuffer(di,  sizeof(DequantInfo));
	ClearBuffer(mi,  sizeof(IMDCTInfo));
	ClearBuffer(sbi, sizeof(SubbandInfo));

	return mp3DecInfo;
}

#define SAFE_FREE(x)	{if (x)	helix_free(x);	(x) = 0;}	/* helper macro */

/**************************************************************************************
 * Function:    FreeBuffers
 *
 * Description: frees all the memory used by the MP3 decoder
 *
 * Inputs:      pointer to initialized MP3DecInfo structure
 *
 * Outputs:     none
 *
 * Return:      none
 *
 * Notes:       safe to call even if some buffers were not allocated (uses SAFE_FREE)
 **************************************************************************************/
void FreeBuffers(MP3DecInfo *mp3DecInfo)
{
	if (!mp3DecInfo)
		return;

	SAFE_FREE(mp3DecInfo->FrameHeaderPS);
	SAFE_FREE(mp3DecInfo->SideInfoPS);
	SAFE_FREE(mp3DecInfo->ScaleFactorInfoPS);
	SAFE_FREE(mp3DecInfo->HuffmanInfoPS);
	SAFE_FREE(mp3DecInfo->DequantInfoPS);
	SAFE_FREE(mp3DecInfo->IMDCTInfoPS);
	SAFE_FREE(mp3DecInfo->SubbandInfoPS);

	SAFE_FREE(mp3DecInfo);
}

/**************************************************************************************
 * Function:    ResetBuffers   (WiPhone addition, 0.9.79 - not in upstream helix)
 *
 * Description: put a decoder back into the state AllocateBuffers() left it in, WITHOUT
 *              freeing and re-allocating its ~29 KB (which is how PSRAM gets fragmented
 *              when it is done per track, per pause and per resume).
 *
 * Why it exists: every piece of history helix keeps from the previous frame - the bit
 *              reservoir (mainBuf / mainDataBytes), the IMDCT overlap, the polyphase
 *              filterbank - belongs to wherever the stream WAS. After a seek (a resume,
 *              a new track) the first frame's main_data_begin then points back into the
 *              OLD position's bytes, which helix decodes without complaint: the "loud
 *              burst" on 2-33 of every 500 replayed resumes (docs/HANDOFF.md, 2026-09-25).
 *              Cleared, the first frame reports ERR_MP3_MAINDATA_UNDERFLOW instead, which
 *              is the truth, and Mp3Stream steps over it.
 **************************************************************************************/
void ResetBuffers(MP3DecInfo *mp3DecInfo)
{
	void *keep[7];

	if (!mp3DecInfo)
		return;

	keep[0] = mp3DecInfo->FrameHeaderPS;
	keep[1] = mp3DecInfo->SideInfoPS;
	keep[2] = mp3DecInfo->ScaleFactorInfoPS;
	keep[3] = mp3DecInfo->HuffmanInfoPS;
	keep[4] = mp3DecInfo->DequantInfoPS;
	keep[5] = mp3DecInfo->IMDCTInfoPS;
	keep[6] = mp3DecInfo->SubbandInfoPS;

	/* memset, not ClearBuffer: the same zeros, but this runs on every track start and the
	 * byte loop over ~29 KB of PSRAM is measurably slower than the library call. */
	memset(mp3DecInfo, 0, sizeof(MP3DecInfo));

	mp3DecInfo->FrameHeaderPS =     keep[0];
	mp3DecInfo->SideInfoPS =        keep[1];
	mp3DecInfo->ScaleFactorInfoPS = keep[2];
	mp3DecInfo->HuffmanInfoPS =     keep[3];
	mp3DecInfo->DequantInfoPS =     keep[4];
	mp3DecInfo->IMDCTInfoPS =       keep[5];
	mp3DecInfo->SubbandInfoPS =     keep[6];

	if (keep[0]) memset(keep[0], 0, sizeof(FrameHeader));
	if (keep[1]) memset(keep[1], 0, sizeof(SideInfo));
	if (keep[2]) memset(keep[2], 0, sizeof(ScaleFactorInfo));
	if (keep[3]) memset(keep[3], 0, sizeof(HuffmanInfo));
	if (keep[4]) memset(keep[4], 0, sizeof(DequantInfo));
	if (keep[5]) memset(keep[5], 0, sizeof(IMDCTInfo));
	if (keep[6]) memset(keep[6], 0, sizeof(SubbandInfo));
}
