/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2010 Red Hat, Inc.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of
   the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _H_MJPEG_DECODER
#define _H_MJPEG_DECODER

#include "common.h"

#ifdef WIN32
/* We need some hacks to avoid warnings from the jpeg headers */
#define XMD_H
#undef FAR
#endif
extern "C" {
#include <jpeglib.h>
}

extern "C" {
    void mjpeg_skip_input_data(j_decompress_ptr cinfo, long num_bytes);
}

class MJpegDecoder {
public:
    MJpegDecoder(int width, int height, int stride,
                 uint8_t *frame);
    ~MJpegDecoder();

    bool decode_data(uint8_t *data, size_t length);

private:

    friend void mjpeg_skip_input_data(j_decompress_ptr cinfo, long num_bytes);

    void convert_scanline(void);

    struct jpeg_decompress_struct _cinfo;
    struct jpeg_error_mgr _jerr;
    struct jpeg_source_mgr _jsrc;

    uint8_t *_data;
    size_t _data_size;
    size_t _data_start;
    size_t _data_end;
    size_t _extra_skip;

    int _width;
    int _height;
    int _stride;
    uint8_t *_frame;

    int _y;
    uint8_t *_scanline;

    int _state;
};

#endif