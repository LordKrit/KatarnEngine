/*
===========================================================================
Copyright (C) 1999 - 2005, Id Software, Inc.
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
Copyright (C) 2005 - 2015, ioquake3 contributors
Copyright (C) 2013 - 2015, OpenJK contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

// tr_draw.c
#include "tr_local.h"
void RE_TempRawImage_CleanUp(void);

void RE_GetScreenShot(byte* buffer, const int w, const int h)
{
	byte* source = nullptr;
	byte* src = nullptr, * dst = nullptr;
	size_t offset = 0, memcount = 0;
	int padlen = 0;

	int			x = 0, y = 0;
	int			r = 0, g = 0, b = 0;
	float		xScale = 0.0f, yScale = 0.0f;
	int			xx = 0, yy = 0;

	source = (byte*)R_Malloc(glConfig.vidWidth * glConfig.vidHeight * 3 + 16, TAG_TEMP_WORKSPACE, qfalse);
	qglPixelStorei(GL_PACK_ALIGNMENT, 1);
	qglReadPixels(0, 0, glConfig.vidWidth, glConfig.vidHeight, GL_RGB, GL_UNSIGNED_BYTE, source);
	memcount = glConfig.vidWidth * 3 * glConfig.vidHeight;

	// gamma correct
	if (glConfig.deviceSupportsGamma)
		R_GammaCorrect(source + offset, memcount);

	// resample from source
	xScale = glConfig.vidWidth / (4.0 * w);
	yScale = glConfig.vidHeight / (3.0 * h);
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			r = g = b = 0;
			for (yy = 0; yy < 3; yy++) {
				for (xx = 0; xx < 4; xx++) {
					src = source + offset + 3 * (glConfig.vidWidth * (int)((y * 3 + yy) * yScale) + (int)((x * 4 + xx) * xScale));
					r += src[0];
					g += src[1];
					b += src[2];
				}
			}
			dst = buffer + 4 * ((h - y - 1) * w + x);
			dst[0] = r / 12;
			dst[1] = g / 12;
			dst[2] = b / 12;
			dst[3] = 255; // OpenJK e791a477: DrawStretchRaw needs a solid alpha
		}
	}

	Z_Free(source);
}

// this is just a chunk of code from RE_TempRawImage_ReadFromFile() below, subroutinised so I can call it
//	from the screen dissolve code as well...
//
static byte* RE_ReSample(byte* pbLoadedPic, const int iLoadedWidth, const int iLoadedHeight, byte* pb_re_sample_buffer, int* piWidth, int* piHeight)
{
	byte* pbReturn = nullptr;

	// if not resampling, just return some values and return...
	//
	if (pb_re_sample_buffer == nullptr || (iLoadedWidth == *piWidth && iLoadedHeight == *piHeight))
	{
		// if not resampling, we're done, just return the loaded size...
		//
		*piWidth = iLoadedWidth;
		*piHeight = iLoadedHeight;
		pbReturn = pbLoadedPic;
	}
	else
	{
		// resample from pbLoadedPic to pbReSampledBuffer...
		//
		float	fXStep = (float)iLoadedWidth / (float)*piWidth;
		float	fYStep = (float)iLoadedHeight / (float)*piHeight;
		int		iTotPixelsPerDownSample = (int)ceil(fXStep) * (int)ceil(fYStep);

		int 	r = 0, g = 0, b = 0;

		byte* pbDst = pb_re_sample_buffer;

		for (int y = 0; y < *piHeight; y++)
		{
			for (int x = 0; x < *piWidth; x++)
			{
				r = g = b = 0;

				for (float yy = (float)y * fYStep; yy < (float)(y + 1) * fYStep; yy += 1)
				{
					for (float xx = (float)x * fXStep; xx < (float)(x + 1) * fXStep; xx += 1)
					{
						byte* pbSrc = pbLoadedPic + 4 * (((int)yy * iLoadedWidth) + (int)xx);

						assert(pbSrc < pbLoadedPic + (iLoadedWidth * iLoadedHeight * 4));

						r += pbSrc[0];
						g += pbSrc[1];
						b += pbSrc[2];
					}
				}

				assert(pbDst < pb_re_sample_buffer + (*piWidth * *piHeight * 4));

				pbDst[0] = r / iTotPixelsPerDownSample;
				pbDst[1] = g / iTotPixelsPerDownSample;
				pbDst[2] = b / iTotPixelsPerDownSample;
				pbDst[3] = 255;
				pbDst += 4;
			}
		}

		// set return value...
		//
		pbReturn = pb_re_sample_buffer;
	}

	return pbReturn;
}

// this is so the server (or anyone else) can get access to raw pixels if they really need to,
//	currently it's only used by the server so that savegames can embed a graphic in the auto-save files
//	(which can't do a screenshot since they're saved out before the level is drawn).
//
// by default, the pic will be returned as the original dims, but if pb_re_sample_buffer != NULL then it's assumed to
//	be a big enough buffer to hold the resampled image, which also means that the width and height params are read as
//	inputs (as well as still being inherently outputs) and the pic is scaled to that size, and to that buffer.
//
// the return value is either NULL, or a pointer to the pixels to use (which may be either the pb_re_sample_buffer param,
//	or the local ptr below).
//
// In either case, you MUST call the free-up function afterwards ( RE_TempRawImage_CleanUp() ) to get rid of any temp
//	memory after you've finished with the pic.
//
// Note: ALWAYS use the return value if != NULL, even if you passed in a declared resample buffer. This is because the
//	resample will get skipped if the values you want are the same size as the pic that it loaded, so it'll return a
//	different buffer.
//
// the vertflip param is used for those functions that expect things in OpenGL's upside-down pixel-read format (sigh)
//
// (not brilliantly fast, but it's only used for weird stuff anyway)
//
byte* pbLoadedPic = NULL;

byte* RE_TempRawImage_ReadFromFile(const char* psLocalFilename, int* piWidth, int* piHeight, byte* pb_re_sample_buffer, const qboolean qbVertFlip)
{
	RE_TempRawImage_CleanUp();	// jic

	byte* pbReturn = NULL;

	if (psLocalFilename && piWidth && piHeight)
	{
		int	 iLoadedWidth = 0, iLoadedHeight = 0;

		R_LoadImage(psLocalFilename, &pbLoadedPic, &iLoadedWidth, &iLoadedHeight);
		if (pbLoadedPic)
		{
			pbReturn = RE_ReSample(pbLoadedPic, iLoadedWidth, iLoadedHeight,
				pb_re_sample_buffer, piWidth, piHeight);
		}
	}

	if (pbReturn && qbVertFlip)
	{
		unsigned int* pSrcLine = (unsigned int*)pbReturn;
		unsigned int* pDstLine = (unsigned int*)pbReturn + (*piHeight * *piWidth);	// *4 done by compiler (longs)
		pDstLine -= *piWidth;	// point at start of last line, not first after buffer

		for (int iLineCount = 0; iLineCount < *piHeight / 2; iLineCount++)
		{
			for (int x = 0; x < *piWidth; x++)
			{
				unsigned int l = pSrcLine[x];
				pSrcLine[x] = pDstLine[x];
				pDstLine[x] = l;
			}
			pSrcLine += *piWidth;
			pDstLine -= *piWidth;
		}
	}

	return pbReturn;
}

void RE_TempRawImage_CleanUp(void)
{
	if (pbLoadedPic)
	{
		R_Free(pbLoadedPic);
		pbLoadedPic = nullptr;
	}
}

// Sojourn: no screen-dissolve wipe (Warzone has no 2D blit path for it); SP just cuts
qboolean RE_ProcessDissolve(void) { return qfalse; }
qboolean RE_InitDissolve(const qboolean bForceCircularExtroWipe) { return qfalse; }
