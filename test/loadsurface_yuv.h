/*
 * Copyright (c) 2008-2009 Intel Corporation. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef _YUVGA_H
#define _YUVGA_H

static int yuvgen_planar(int width, int height,
                         unsigned char *Y_start, int Y_pitch,
                         unsigned char *U_start, int U_pitch,
                         unsigned char *V_start, int V_pitch,
                         unsigned int fourcc, int box_width, int row_shift,
                         int field)
{
    int row;
    unsigned char uv_value = 0x80;

    /* copy Y plane */
    int y_factor = 1;
    if (fourcc == VA_FOURCC_YUY2) y_factor = 2;
    for (row=0;row<height;row++) {
        unsigned char *Y_row = Y_start + row * Y_pitch;
        int jj, ypos;

        ypos = (row / box_width) & 0x1;

        /* fill garbage data into the other field */
        if (((field == VA_TOP_FIELD) && (row &1))
            || ((field == VA_BOTTOM_FIELD) && ((row &1)==0))) { 
            memset(Y_row, 0xff, width);
            continue;
        }
        
        for (jj=0; jj<width; jj++) {
            int xpos = ((row_shift + jj) / box_width) & 0x1;
            if (xpos == ypos)
                Y_row[jj*y_factor] = 0xeb;
            else 
                Y_row[jj*y_factor] = 0x10;

            if (fourcc == VA_FOURCC_YUY2) {
                Y_row[jj*y_factor+1] = uv_value; // it is for UV
            }
        }
    }

    /* copy UV data */
    for( row =0; row < height/2; row++) {

        /* fill garbage data into the other field */
        if (((field == VA_TOP_FIELD) && (row &1))
            || ((field == VA_BOTTOM_FIELD) && ((row &1)==0))) {
            uv_value = 0xff;
        }

        unsigned char *U_row = U_start + row * U_pitch;
        unsigned char *V_row = V_start + row * V_pitch;
        switch (fourcc) {
        case VA_FOURCC_NV12:
            memset(U_row, uv_value, width);
            break;
        case VA_FOURCC_YV12:
            memset (U_row,uv_value,width/2);
            memset (V_row,uv_value,width/2);
            break;
        case VA_FOURCC_YUY2:
            // see above. it is set with Y update.
            break;
        default:
            printf("unsupported fourcc in loadsurface.h\n");
            assert(0);
        }
    }

    return 0;
}


#endif
