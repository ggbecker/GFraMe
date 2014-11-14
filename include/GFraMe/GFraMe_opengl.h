/**
 * @file include/GFraMe/GFraMe_opengl.h
 */
#ifndef __GFRAME_OPENGL_H_
#define __GFRAME_OPENGL_H_

#include <GFraMe/GFraMe_error.h>

GFraMe_ret GFraMe_opengl_init(char *texF, int texW, int texH, int winW,
	int winH, int sX, int sY);

void GFraMe_opengl_clear();

void GFraMe_opengl_setAtt();

void GFraMe_opengl_prepareRender();

void GFraMe_opengl_renderSprite(int x, int y, int d, int tx, int ty);

void GFraMe_opengl_doRender();

#endif