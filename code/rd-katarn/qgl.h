/*
===========================================================================
Copyright (C) 1999 - 2005, Id Software, Inc.
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
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

#pragma once

#if defined( __LINT__ )
#	include <GL/gl.h>
#elif defined( _WIN32 )
#	include <windows.h>
#	include <GL/gl.h>
#elif defined(MACOS_X)
// Prevent OS X headers from including its own glext header
#	define GL_GLEXT_LEGACY
#	include <OpenGL/gl.h>
#elif defined( __linux__ )
#	include <GL/gl.h>
#	include <GL/glx.h>
// bk001129 - from cvs1.17 (mkv)
#	if defined(__FX__)
#		include <GL/fxmesa.h>
#	endif
#elif defined( __FreeBSD__ ) // rb010123
#	include <GL/gl.h>
#	include <GL/glx.h>
#	if defined(__FX__)
#		include <GL/fxmesa.h>
#	endif
#else
#	include <gl.h>
#endif

#include "glext.h"

// KatarnEngine: the fixed-function GL calls of the vanilla front end are translated to D3D12 (katarn/kt_gl.cpp)
void APIENTRY ktgl_AlphaFunc(GLenum func,GLclampf ref);
void APIENTRY ktgl_ArrayElement(GLint i);
void APIENTRY ktgl_Begin(GLenum mode);
void APIENTRY ktgl_BindTexture(GLenum target,GLuint texture);
void APIENTRY ktgl_BlendFunc(GLenum sfactor,GLenum dfactor);
void APIENTRY ktgl_CallList(GLuint list);
void APIENTRY ktgl_Clear(GLbitfield mask);
void APIENTRY ktgl_ClearColor(GLclampf red,GLclampf green,GLclampf blue,GLclampf alpha);
void APIENTRY ktgl_ClearDepth(GLclampd depth);
void APIENTRY ktgl_ClearStencil(GLint s);
void APIENTRY ktgl_ClipPlane(GLenum plane,const GLdouble *equation);
void APIENTRY ktgl_Color3f(GLfloat red,GLfloat green,GLfloat blue);
void APIENTRY ktgl_Color4f(GLfloat red,GLfloat green,GLfloat blue,GLfloat alpha);
void APIENTRY ktgl_Color4ub(GLubyte red,GLubyte green,GLubyte blue,GLubyte alpha);
void APIENTRY ktgl_Color4ubv(const GLubyte *v);
void APIENTRY ktgl_ColorMask(GLboolean red,GLboolean green,GLboolean blue,GLboolean alpha);
void APIENTRY ktgl_ColorPointer(GLint size,GLenum type,GLsizei stride,const GLvoid *pointer);
void APIENTRY ktgl_CopyTexImage2D(GLenum target,GLint level,GLenum internalFormat,GLint x,GLint y,GLsizei width,GLsizei height,GLint border);
void APIENTRY ktgl_CopyTexSubImage2D(GLenum target,GLint level,GLint xoffset,GLint yoffset,GLint x,GLint y,GLsizei width,GLsizei height);
void APIENTRY ktgl_CullFace(GLenum mode);
void APIENTRY ktgl_DeleteLists(GLuint list,GLsizei range);
void APIENTRY ktgl_DeleteTextures(GLsizei n,const GLuint *textures);
void APIENTRY ktgl_DepthFunc(GLenum func);
void APIENTRY ktgl_DepthMask(GLboolean flag);
void APIENTRY ktgl_DepthRange(GLclampd zNear,GLclampd zFar);
void APIENTRY ktgl_Disable(GLenum cap);
void APIENTRY ktgl_DisableClientState(GLenum array);
void APIENTRY ktgl_DrawArrays(GLenum mode,GLint first,GLsizei count);
void APIENTRY ktgl_DrawBuffer(GLenum mode);
void APIENTRY ktgl_DrawElements(GLenum mode,GLsizei count,GLenum type,const GLvoid *indices);
void APIENTRY ktgl_Enable(GLenum cap);
void APIENTRY ktgl_EnableClientState(GLenum array);
void APIENTRY ktgl_End(void);
void APIENTRY ktgl_Finish(void);
void APIENTRY ktgl_Fogf(GLenum pname,GLfloat param);
void APIENTRY ktgl_Fogfv(GLenum pname,const GLfloat *params);
void APIENTRY ktgl_Fogi(GLenum pname,GLint param);
void APIENTRY ktgl_GenTextures(GLsizei n,GLuint *textures);
void APIENTRY ktgl_GetDoublev(GLenum pname,GLdouble *params);
GLenum APIENTRY ktgl_GetError(void);
void APIENTRY ktgl_GetFloatv(GLenum pname,GLfloat *params);
void APIENTRY ktgl_GetIntegerv(GLenum pname,GLint *params);
const GLubyte * APIENTRY ktgl_GetString(GLenum name);
GLboolean APIENTRY ktgl_IsEnabled(GLenum cap);
void APIENTRY ktgl_LineWidth(GLfloat width);
void APIENTRY ktgl_LoadIdentity(void);
void APIENTRY ktgl_LoadMatrixf(const GLfloat *m);
void APIENTRY ktgl_MatrixMode(GLenum mode);
void APIENTRY ktgl_Ortho(GLdouble left,GLdouble right,GLdouble bottom,GLdouble top,GLdouble zNear,GLdouble zFar);
void APIENTRY ktgl_PixelStorei(GLenum pname,GLint param);
void APIENTRY ktgl_PolygonMode(GLenum face,GLenum mode);
void APIENTRY ktgl_PolygonOffset(GLfloat factor,GLfloat units);
void APIENTRY ktgl_PopAttrib(void);
void APIENTRY ktgl_PopMatrix(void);
void APIENTRY ktgl_PushAttrib(GLbitfield mask);
void APIENTRY ktgl_PushMatrix(void);
void APIENTRY ktgl_ReadPixels(GLint x,GLint y,GLsizei width,GLsizei height,GLenum format,GLenum type,GLvoid *pixels);
void APIENTRY ktgl_Scissor(GLint x,GLint y,GLsizei width,GLsizei height);
void APIENTRY ktgl_ShadeModel(GLenum mode);
void APIENTRY ktgl_StencilFunc(GLenum func,GLint ref,GLuint mask);
void APIENTRY ktgl_StencilMask(GLuint mask);
void APIENTRY ktgl_StencilOp(GLenum fail,GLenum zfail,GLenum zpass);
void APIENTRY ktgl_TexCoord2f(GLfloat s,GLfloat t);
void APIENTRY ktgl_TexCoord2fv(const GLfloat *v);
void APIENTRY ktgl_TexCoordPointer(GLint size,GLenum type,GLsizei stride,const GLvoid *pointer);
void APIENTRY ktgl_TexEnvf(GLenum target,GLenum pname,GLfloat param);
void APIENTRY ktgl_TexImage2D(GLenum target,GLint level,GLint internalformat,GLsizei width,GLsizei height,GLint border,GLenum format,GLenum type,const GLvoid *pixels);
void APIENTRY ktgl_TexParameterf(GLenum target,GLenum pname,GLfloat param);
void APIENTRY ktgl_TexParameterfv(GLenum target,GLenum pname,const GLfloat *params);
void APIENTRY ktgl_TexParameteri(GLenum target,GLenum pname,GLint param);
void APIENTRY ktgl_TexSubImage2D(GLenum target,GLint level,GLint xoffset,GLint yoffset,GLsizei width,GLsizei height,GLenum format,GLenum type,const GLvoid *pixels);
void APIENTRY ktgl_Translatef(GLfloat x,GLfloat y,GLfloat z);
void APIENTRY ktgl_Vertex2f(GLfloat x,GLfloat y);
void APIENTRY ktgl_Vertex3f(GLfloat x,GLfloat y,GLfloat z);
void APIENTRY ktgl_Vertex3fv(const GLfloat *v);
void APIENTRY ktgl_VertexPointer(GLint size,GLenum type,GLsizei stride,const GLvoid *pointer);
void APIENTRY ktgl_Viewport(GLint x,GLint y,GLsizei width,GLsizei height);

#define qglAccum glAccum
#define qglAlphaFunc ktgl_AlphaFunc
#define qglAreTexturesResident glAreTexturesResident
#define qglArrayElement ktgl_ArrayElement
#define qglBegin ktgl_Begin
#define qglBindTexture ktgl_BindTexture
#define qglBitmap glBitmap
#define qglBlendFunc ktgl_BlendFunc
#define qglCallList ktgl_CallList
#define qglCallLists glCallLists
#define qglClear ktgl_Clear
#define qglClearAccum glClearAccum
#define qglClearColor ktgl_ClearColor
#define qglClearDepth ktgl_ClearDepth
#define qglClearIndex glClearIndex
#define qglClearStencil ktgl_ClearStencil
#define qglClipPlane ktgl_ClipPlane
#define qglColor3b glColor3b
#define qglColor3bv glColor3bv
#define qglColor3d glColor3d
#define qglColor3dv glColor3dv
#define qglColor3f ktgl_Color3f
#define qglColor3fv glColor3fv
#define qglColor3i glColor3i
#define qglColor3iv glColor3iv
#define qglColor3s glColor3s
#define qglColor3sv glColor3sv
#define qglColor3ub glColor3ub
#define qglColor3ubv glColor3ubv
#define qglColor3ui glColor3ui
#define qglColor3uiv glColor3uiv
#define qglColor3us glColor3us
#define qglColor3usv glColor3usv
#define qglColor4b glColor4b
#define qglColor4bv glColor4bv
#define qglColor4d glColor4d
#define qglColor4dv glColor4dv
#define qglColor4f ktgl_Color4f
#define qglColor4fv glColor4fv
#define qglColor4i glColor4i
#define qglColor4iv glColor4iv
#define qglColor4s glColor4s
#define qglColor4sv glColor4sv
#define qglColor4ub ktgl_Color4ub
#define qglColor4ubv ktgl_Color4ubv
#define qglColor4ui glColor4ui
#define qglColor4uiv glColor4uiv
#define qglColor4us glColor4us
#define qglColor4usv glColor4usv
#define qglColorMask ktgl_ColorMask
#define qglColorMaterial glColorMaterial
#define qglColorPointer ktgl_ColorPointer
#define qglCopyPixels glCopyPixels
#define qglCopyTexImage1D glCopyTexImage1D
#define qglCopyTexImage2D ktgl_CopyTexImage2D
#define qglCopyTexSubImage1D glCopyTexSubImage1D
#define qglCopyTexSubImage2D ktgl_CopyTexSubImage2D
#define qglCullFace ktgl_CullFace
#define qglDeleteLists ktgl_DeleteLists
#define qglDeleteTextures ktgl_DeleteTextures
#define qglDepthFunc ktgl_DepthFunc
#define qglDepthMask ktgl_DepthMask
#define qglDepthRange ktgl_DepthRange
#define qglDisable ktgl_Disable
#define qglDisableClientState ktgl_DisableClientState
#define qglDrawArrays ktgl_DrawArrays
#define qglDrawBuffer ktgl_DrawBuffer
#define qglDrawElements ktgl_DrawElements
#define qglDrawPixels glDrawPixels
#define qglEdgeFlag glEdgeFlag
#define qglEdgeFlagPointer glEdgeFlagPointer
#define qglEdgeFlagv glEdgeFlagv
#define qglEnable ktgl_Enable
#define qglEnableClientState ktgl_EnableClientState
#define qglEnd ktgl_End
#define qglEndList glEndList
#define qglEvalCoord1d glEvalCoord1d
#define qglEvalCoord1dv glEvalCoord1dv
#define qglEvalCoord1f glEvalCoord1f
#define qglEvalCoord1fv glEvalCoord1fv
#define qglEvalCoord2d glEvalCoord2d
#define qglEvalCoord2dv glEvalCoord2dv
#define qglEvalCoord2f glEvalCoord2f
#define qglEvalCoord2fv glEvalCoord2fv
#define qglEvalMesh1 glEvalMesh1
#define qglEvalMesh2 glEvalMesh2
#define qglEvalPoint1 glEvalPoint1
#define qglEvalPoint2 glEvalPoint2
#define qglFeedbackBuffer glFeedbackBuffer
#define qglFinish ktgl_Finish
#define qglFlush glFlush
#define qglFogf ktgl_Fogf
#define qglFogfv ktgl_Fogfv
#define qglFogi ktgl_Fogi
#define qglFogiv glFogiv
#define qglFrontFace glFrontFace
#define qglFrustum glFrustum
#define qglGenLists glGenLists
#define qglGenTextures ktgl_GenTextures
#define qglGetBooleanv glGetBooleanv
#define qglGetClipPlane glGetClipPlane
#define qglGetDoublev ktgl_GetDoublev
#define qglGetError ktgl_GetError
#define qglGetFloatv ktgl_GetFloatv
#define qglGetIntegerv ktgl_GetIntegerv
#define qglGetLightfv glGetLightfv
#define qglGetLightiv glGetLightiv
#define qglGetMapdv glGetMapdv
#define qglGetMapfv glGetMapfv
#define qglGetMapiv glGetMapiv
#define qglGetMaterialfv glGetMaterialfv
#define qglGetMaterialiv glGetMaterialiv
#define qglGetPixelMapfv glGetPixelMapfv
#define qglGetPixelMapuiv glGetPixelMapuiv
#define qglGetPixelMapusv glGetPixelMapusv
#define qglGetPointerv glGetPointerv
#define qglGetPolygonStipple glGetPolygonStipple
#define qglGetString ktgl_GetString
#define qglGetTexGendv glGetTexGendv
#define qglGetTexGenfv glGetTexGenfv
#define qglGetTexGeniv glGetTexGeniv
#define qglGetTexImage glGetTexImage
#define qglGetTexLevelParameterfv glGetTexLevelParameterfv
#define qglGetTexLevelParameteriv glGetTexLevelParameteriv
#define qglGetTexParameterfv glGetTexParameterfv
#define qglGetTexParameteriv glGetTexParameteriv
#define qglHint glHint
#define qglIndexMask glIndexMask
#define qglIndexPointer glIndexPointer
#define qglIndexd glIndexd
#define qglIndexdv glIndexdv
#define qglIndexf glIndexf
#define qglIndexfv glIndexfv
#define qglIndexi glIndexi
#define qglIndexiv glIndexiv
#define qglIndexs glIndexs
#define qglIndexsv glIndexsv
#define qglIndexub glIndexub
#define qglIndexubv glIndexubv
#define qglInitNames glInitNames
#define qglInterleavedArrays glInterleavedArrays
#define qglIsEnabled ktgl_IsEnabled
#define qglIsList glIsList
#define qglIsTexture glIsTexture
#define qglLightModelf glLightModelf
#define qglLightModelfv glLightModelfv
#define qglLightModeli glLightModeli
#define qglLightModeliv glLightModeliv
#define qglLightf glLightf
#define qglLightfv glLightfv
#define qglLighti glLighti
#define qglLightiv glLightiv
#define qglLineStipple glLineStipple
#define qglLineWidth ktgl_LineWidth
#define qglListBase glListBase
#define qglLoadIdentity ktgl_LoadIdentity
#define qglLoadMatrixd glLoadMatrixd
#define qglLoadMatrixf ktgl_LoadMatrixf
#define qglLoadName glLoadName
#define qglLogicOp glLogicOp
#define qglMap1d glMap1d
#define qglMap1f glMap1f
#define qglMap2d glMap2d
#define qglMap2f glMap2f
#define qglMapGrid1d glMapGrid1d
#define qglMapGrid1f glMapGrid1f
#define qglMapGrid2d glMapGrid2d
#define qglMapGrid2f glMapGrid2f
#define qglMaterialf glMaterialf
#define qglMaterialfv glMaterialfv
#define qglMateriali glMateriali
#define qglMaterialiv glMaterialiv
#define qglMatrixMode ktgl_MatrixMode
#define qglMultMatrixd glMultMatrixd
#define qglMultMatrixf glMultMatrixf
#define qglNewList glNewList
#define qglNormal3b glNormal3b
#define qglNormal3bv glNormal3bv
#define qglNormal3d glNormal3d
#define qglNormal3dv glNormal3dv
#define qglNormal3f glNormal3f
#define qglNormal3fv glNormal3fv
#define qglNormal3i glNormal3i
#define qglNormal3iv glNormal3iv
#define qglNormal3s glNormal3s
#define qglNormal3sv glNormal3sv
#define qglNormalPointer glNormalPointer
#define qglOrtho ktgl_Ortho
#define qglPassThrough glPassThrough
#define qglPixelMapfv glPixelMapfv
#define qglPixelMapuiv glPixelMapuiv
#define qglPixelMapusv glPixelMapusv
#define qglPixelStoref glPixelStoref
#define qglPixelStorei ktgl_PixelStorei
#define qglPixelTransferf glPixelTransferf
#define qglPixelTransferi glPixelTransferi
#define qglPixelZoom glPixelZoom
#define qglPointSize glPointSize
#define qglPolygonMode ktgl_PolygonMode
#define qglPolygonOffset ktgl_PolygonOffset
#define qglPolygonStipple glPolygonStipple
#define qglPopAttrib ktgl_PopAttrib
#define qglPopClientAttrib glPopClientAttrib
#define qglPopMatrix ktgl_PopMatrix
#define qglPopName glPopName
#define qglPrioritizeTextures glPrioritizeTextures
#define qglPushAttrib ktgl_PushAttrib
#define qglPushClientAttrib glPushClientAttrib
#define qglPushMatrix ktgl_PushMatrix
#define qglPushName glPushName
#define qglRasterPos2d glRasterPos2d
#define qglRasterPos2dv glRasterPos2dv
#define qglRasterPos2f glRasterPos2f
#define qglRasterPos2fv glRasterPos2fv
#define qglRasterPos2i glRasterPos2i
#define qglRasterPos2iv glRasterPos2iv
#define qglRasterPos2s glRasterPos2s
#define qglRasterPos2sv glRasterPos2sv
#define qglRasterPos3d glRasterPos3d
#define qglRasterPos3dv glRasterPos3dv
#define qglRasterPos3f glRasterPos3f
#define qglRasterPos3fv glRasterPos3fv
#define qglRasterPos3i glRasterPos3i
#define qglRasterPos3iv glRasterPos3iv
#define qglRasterPos3s glRasterPos3s
#define qglRasterPos3sv glRasterPos3sv
#define qglRasterPos4d glRasterPos4d
#define qglRasterPos4dv glRasterPos4dv
#define qglRasterPos4f glRasterPos4f
#define qglRasterPos4fv glRasterPos4fv
#define qglRasterPos4i glRasterPos4i
#define qglRasterPos4iv glRasterPos4iv
#define qglRasterPos4s glRasterPos4s
#define qglRasterPos4sv glRasterPos4sv
#define qglReadBuffer glReadBuffer
#define qglReadPixels ktgl_ReadPixels
#define qglRectd glRectd
#define qglRectdv glRectdv
#define qglRectf glRectf
#define qglRectfv glRectfv
#define qglRecti glRecti
#define qglRectiv glRectiv
#define qglRects glRects
#define qglRectsv glRectsv
#define qglRenderMode glRenderMode
#define qglRotated glRotated
#define qglRotatef glRotatef
#define qglScaled glScaled
#define qglScalef glScalef
#define qglScissor ktgl_Scissor
#define qglSelectBuffer glSelectBuffer
#define qglShadeModel ktgl_ShadeModel
#define qglStencilFunc ktgl_StencilFunc
#define qglStencilMask ktgl_StencilMask
#define qglStencilOp ktgl_StencilOp
#if defined(__APPLE__)
#define qglStencilOpSeparate glStencilOpSeparate
#endif
#define qglTexCoord1d glTexCoord1d
#define qglTexCoord1dv glTexCoord1dv
#define qglTexCoord1f glTexCoord1f
#define qglTexCoord1fv glTexCoord1fv
#define qglTexCoord1i glTexCoord1i
#define qglTexCoord1iv glTexCoord1iv
#define qglTexCoord1s glTexCoord1s
#define qglTexCoord1sv glTexCoord1sv
#define qglTexCoord2d glTexCoord2d
#define qglTexCoord2dv glTexCoord2dv
#define qglTexCoord2f ktgl_TexCoord2f
#define qglTexCoord2fv ktgl_TexCoord2fv
#define qglTexCoord2i glTexCoord2i
#define qglTexCoord2iv glTexCoord2iv
#define qglTexCoord2s glTexCoord2s
#define qglTexCoord2sv glTexCoord2sv
#define qglTexCoord3d glTexCoord3d
#define qglTexCoord3dv glTexCoord3dv
#define qglTexCoord3f glTexCoord3f
#define qglTexCoord3fv glTexCoord3fv
#define qglTexCoord3i glTexCoord3i
#define qglTexCoord3iv glTexCoord3iv
#define qglTexCoord3s glTexCoord3s
#define qglTexCoord3sv glTexCoord3sv
#define qglTexCoord4d glTexCoord4d
#define qglTexCoord4dv glTexCoord4dv
#define qglTexCoord4f glTexCoord4f
#define qglTexCoord4fv glTexCoord4fv
#define qglTexCoord4i glTexCoord4i
#define qglTexCoord4iv glTexCoord4iv
#define qglTexCoord4s glTexCoord4s
#define qglTexCoord4sv glTexCoord4sv
#define qglTexCoordPointer ktgl_TexCoordPointer
#define qglTexEnvf ktgl_TexEnvf
#define qglTexEnvfv glTexEnvfv
#define qglTexEnvi glTexEnvi
#define qglTexEnviv glTexEnviv
#define qglTexGend glTexGend
#define qglTexGendv glTexGendv
#define qglTexGenf glTexGenf
#define qglTexGenfv glTexGenfv
#define qglTexGeni glTexGeni
#define qglTexGeniv glTexGeniv
#define qglTexImage1D glTexImage1D
#define qglTexImage2D ktgl_TexImage2D
#if defined(__APPLE__)
#define qglTexImage3D glTexImage3D
#define qglTexSubImage3D glTexSubImage3D
#endif
#define qglTexParameterf ktgl_TexParameterf
#define qglTexParameterfv ktgl_TexParameterfv
#define qglTexParameteri ktgl_TexParameteri
#define qglTexParameteriv glTexParameteriv
#define qglTexSubImage1D glTexSubImage1D
#define qglTexSubImage2D ktgl_TexSubImage2D
#define qglTranslated glTranslated
#define qglTranslatef ktgl_Translatef
#define qglVertex2d glVertex2d
#define qglVertex2dv glVertex2dv
#define qglVertex2f ktgl_Vertex2f
#define qglVertex2fv glVertex2fv
#define qglVertex2i glVertex2i
#define qglVertex2iv glVertex2iv
#define qglVertex2s glVertex2s
#define qglVertex2sv glVertex2sv
#define qglVertex3d glVertex3d
#define qglVertex3dv glVertex3dv
#define qglVertex3f ktgl_Vertex3f
#define qglVertex3fv ktgl_Vertex3fv
#define qglVertex3i glVertex3i
#define qglVertex3iv glVertex3iv
#define qglVertex3s glVertex3s
#define qglVertex3sv glVertex3sv
#define qglVertex4d glVertex4d
#define qglVertex4dv glVertex4dv
#define qglVertex4f glVertex4f
#define qglVertex4fv glVertex4fv
#define qglVertex4i glVertex4i
#define qglVertex4iv glVertex4iv
#define qglVertex4s glVertex4s
#define qglVertex4sv glVertex4sv
#define qglVertexPointer ktgl_VertexPointer
#define qglViewport ktgl_Viewport

#if !defined(__APPLE__)
extern PFNGLSTENCILOPSEPARATEPROC qglStencilOpSeparate;
#endif

extern PFNGLACTIVETEXTUREARBPROC qglActiveTextureARB;
extern PFNGLCLIENTACTIVETEXTUREARBPROC qglClientActiveTextureARB;
extern PFNGLMULTITEXCOORD2FARBPROC qglMultiTexCoord2fARB;
#if !defined(__APPLE__)
// SJE: 3D textures for the r_gammaShaders lookup table
extern PFNGLTEXIMAGE3DPROC qglTexImage3D;
extern PFNGLTEXSUBIMAGE3DPROC qglTexSubImage3D;
#endif

extern PFNGLCOMBINERPARAMETERFVNVPROC qglCombinerParameterfvNV;
extern PFNGLCOMBINERPARAMETERIVNVPROC qglCombinerParameterivNV;
extern PFNGLCOMBINERPARAMETERFNVPROC qglCombinerParameterfNV;
extern PFNGLCOMBINERPARAMETERINVPROC qglCombinerParameteriNV;
extern PFNGLCOMBINERINPUTNVPROC qglCombinerInputNV;
extern PFNGLCOMBINEROUTPUTNVPROC qglCombinerOutputNV;

extern PFNGLFINALCOMBINERINPUTNVPROC qglFinalCombinerInputNV;
extern PFNGLGETCOMBINERINPUTPARAMETERFVNVPROC qglGetCombinerInputParameterfvNV;
extern PFNGLGETCOMBINERINPUTPARAMETERIVNVPROC qglGetCombinerInputParameterivNV;
extern PFNGLGETCOMBINEROUTPUTPARAMETERFVNVPROC qglGetCombinerOutputParameterfvNV;
extern PFNGLGETCOMBINEROUTPUTPARAMETERIVNVPROC qglGetCombinerOutputParameterivNV;
extern PFNGLGETFINALCOMBINERINPUTPARAMETERFVNVPROC qglGetFinalCombinerInputParameterfvNV;
extern PFNGLGETFINALCOMBINERINPUTPARAMETERIVNVPROC qglGetFinalCombinerInputParameterivNV;

extern PFNGLPROGRAMSTRINGARBPROC qglProgramStringARB;
extern PFNGLBINDPROGRAMARBPROC qglBindProgramARB;
extern PFNGLDELETEPROGRAMSARBPROC qglDeleteProgramsARB;
extern PFNGLGENPROGRAMSARBPROC qglGenProgramsARB;
extern PFNGLPROGRAMENVPARAMETER4DARBPROC qglProgramEnvParameter4dARB;
extern PFNGLPROGRAMENVPARAMETER4DVARBPROC qglProgramEnvParameter4dvARB;
extern PFNGLPROGRAMENVPARAMETER4FARBPROC qglProgramEnvParameter4fARB;
extern PFNGLPROGRAMENVPARAMETER4FVARBPROC qglProgramEnvParameter4fvARB;
extern PFNGLPROGRAMLOCALPARAMETER4DARBPROC qglProgramLocalParameter4dARB;
extern PFNGLPROGRAMLOCALPARAMETER4DVARBPROC qglProgramLocalParameter4dvARB;
extern PFNGLPROGRAMLOCALPARAMETER4FARBPROC qglProgramLocalParameter4fARB;
extern PFNGLPROGRAMLOCALPARAMETER4FVARBPROC qglProgramLocalParameter4fvARB;
extern PFNGLGETPROGRAMENVPARAMETERDVARBPROC qglGetProgramEnvParameterdvARB;
extern PFNGLGETPROGRAMENVPARAMETERFVARBPROC qglGetProgramEnvParameterfvARB;
extern PFNGLGETPROGRAMLOCALPARAMETERDVARBPROC qglGetProgramLocalParameterdvARB;
extern PFNGLGETPROGRAMLOCALPARAMETERFVARBPROC qglGetProgramLocalParameterfvARB;
extern PFNGLGETPROGRAMIVARBPROC qglGetProgramivARB;
extern PFNGLGETPROGRAMSTRINGARBPROC qglGetProgramStringARB;
extern PFNGLISPROGRAMARBPROC qglIsProgramARB;

extern PFNGLLOCKARRAYSEXTPROC qglLockArraysEXT;
extern PFNGLUNLOCKARRAYSEXTPROC qglUnlockArraysEXT;
