/* Copyright (c) 2018-2021, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <string.h>
#include "canvas.h"
#include "sds.h"

/* ========================== ASCII Canvas =============================== */

/* Allocate and return a new canvas of the specified size. */
canvas *createCanvas(int width, int height, int bgcolor) {
    canvas *canvas = malloc(sizeof(*canvas));
    canvas->width = width;
    canvas->height = height;
    canvas->pixels = malloc(width*height);
    memset(canvas->pixels,bgcolor,width*height);
    return canvas;
}

/* Free the canvas created by createCanvas(). */
void freeCanvas(canvas *canvas) {
    free(canvas->pixels);
    free(canvas);
}

/* Set a pixel to the specified color. Color is 0 or 1, where zero means no
 * dot will be displyed, and 1 means dot will be displayed.
 * Coordinates are arranged so that left-top corner is 0,0. You can write
 * out of the size of the canvas without issues. */
void drawPixel(canvas *canvas, int x, int y, int color) {
    if (x < 0 || x >= canvas->width ||
        y < 0 || y >= canvas->height) return;
    canvas->pixels[x+y*canvas->width] = color;
}

/* Return the value of the specified pixel on the canvas. */
int getPixel(canvas *canvas, int x, int y) {
    if (x < 0 || x >= canvas->width ||
        y < 0 || y >= canvas->height) return 0;
    return canvas->pixels[x+y*canvas->width];
}

/* Draw a line from x1,y1 to x2,y2 using the Bresenham algorithm. */
void drawLine(canvas *canvas, int x1, int y1, int x2, int y2, int color) {
    int dx = abs(x2-x1);
    int dy = abs(y2-y1);
    int sx = (x1 < x2) ? 1 : -1;
    int sy = (y1 < y2) ? 1 : -1;
    int err = dx-dy, e2;

    while(1) {
        drawPixel(canvas,x1,y1,color);
        if (x1 == x2 && y1 == y2) break;
        e2 = err*2;
        if (e2 > -dy) {
            err -= dy;
            x1 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y1 += sy;
        }
    }
}

/* Translate a group of 8 pixels (2x4 vertical rectangle) to the corresponding
 * braille character. The byte should correspond to the pixels arranged as
 * follows, where 0 is the least significant bit, and 7 the most significant
 * bit:
 *
 *   0 3
 *   1 4
 *   2 5
 *   6 7
 *
 * The corresponding utf8 encoded character is set into the three bytes
 * pointed by 'output'.
 */
#include <stdio.h>
static void brailleTranslatePixelsGroup(int byte, char *output) {
    int code = 0x2800 + byte;
    /* Convert to unicode. This is in the U0800-UFFFF range, so we need to
     * emit it like this in three bytes:
     * 1110xxxx 10xxxxxx 10xxxxxx. */
    output[0] = 0xE0 | (code >> 12);          /* 1110-xxxx */
    output[1] = 0x80 | ((code >> 6) & 0x3F);  /* 10-xxxxxx */
    output[2] = 0x80 | (code & 0x3F);         /* 10-xxxxxx */
}

/* Converts the canvas to an SDS string representing the UTF8 characters to
 * print to the terminal in order to obtain a graphical representaiton of the
 * logical canvas. The actual returned string will require a terminal that is
 * width/2 large and height/4 tall in order to hold the whole image without
 * overflowing or scrolling, since each Barille character is 2x4. */
sds renderCanvas(canvas *canvas) {
    sds text = sdsempty();
    for (int y = 0; y < canvas->height; y += 4) {
        for (int x = 0; x < canvas->width; x += 2) {
            /* We need to emit groups of 8 bits according to a specific
             * arrangement. See lwTranslatePixelsGroup() for more info. */
            int byte = 0;
            if (getPixel(canvas,x,y)) byte |= (1<<0);
            if (getPixel(canvas,x,y+1)) byte |= (1<<1);
            if (getPixel(canvas,x,y+2)) byte |= (1<<2);
            if (getPixel(canvas,x+1,y)) byte |= (1<<3);
            if (getPixel(canvas,x+1,y+1)) byte |= (1<<4);
            if (getPixel(canvas,x+1,y+2)) byte |= (1<<5);
            if (getPixel(canvas,x,y+3)) byte |= (1<<6);
            if (getPixel(canvas,x+1,y+3)) byte |= (1<<7);
            char unicode[3];
            brailleTranslatePixelsGroup(byte,unicode);
            text = sdscatlen(text,unicode,3);
        }
        if (y != canvas->height-1) text = sdscatlen(text,"\n",1);
    }
    return text;
}
