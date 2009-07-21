/*
  pygame - Python Game Library
  Copyright (C) 2009 Vicent Marti

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Library General Public
  License as published by the Free Software Foundation; either
  version 2 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Library General Public License for more details.

  You should have received a copy of the GNU Library General Public
  License along with this library; if not, write to the Free
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

*/

#define PYGAME_FREETYPE_INTERNAL

#include "ft_mod.h"
#include "ft_wrap.h"
#include "pgfreetype.h"
#include "pgtypes.h"
#include "freetypebase_doc.h"

#include FT_MODULE_H

#ifdef HAVE_PYGAME_SDL_VIDEO
#   include "surface.h"
#endif

typedef void (* FontRenderPtr)(int, int, FontSurface *, FT_Bitmap *, PyColor *);
typedef void (* FontFillPtr)(int, int, int, int, FontSurface *, PyColor *);

#define SLANT_FACTOR    0.22
FT_Matrix PGFT_SlantMatrix = 
{
    (1 << 16),  (FT_Fixed)(SLANT_FACTOR * (1 << 16)),
    0,          (1 << 16) 
};

FT_Fixed PGFT_GetBoldStrength(FT_Face face)
{
    const float bold_factor = 0.06f;
    FT_Fixed bold_str;

    bold_str = FT_MulFix(face->units_per_EM, face->size->metrics.y_scale);
    bold_str = (FT_Fixed)(bold_str * bold_factor);

    return bold_str;
}

void 
PGFT_BuildRenderMode(FontRenderMode *mode, int style, int vertical, int antialias, int rotation)
{
    double      radian;
    FT_Fixed    cosinus;
    FT_Fixed    sinus;
    int         angle;

    angle = rotation % 360;
    while (angle < 0) angle += 360;

    mode->kerning_mode = 1;
    mode->kerning_degree = 0;

    mode->style = (FT_Byte)style;

    mode->vertical = (FT_Byte)vertical;
    mode->hinted = (FT_Byte)1;
    mode->autohint = 0;
    mode->antialias = (FT_Byte)antialias;
    mode->matrix = NULL;
    mode->_rotation_angle = 0;

    if (angle != 0)
    {
        radian  = angle * 3.14159 / 180.0;
        cosinus = (FT_Fixed)(cos(radian) * 65536.0);
        sinus   = (FT_Fixed)(sin(radian) * 65536.0);

        mode->_rotation_matrix.xx = cosinus;
        mode->_rotation_matrix.yx = sinus;
        mode->_rotation_matrix.xy = -sinus;
        mode->_rotation_matrix.yy = cosinus;
        mode->_rotation_angle = (angle << 16);

        mode->matrix = &mode->_rotation_matrix;
    }
}


/*********************************************************
 *
 * Rendering on SDL-specific surfaces
 *
 *********************************************************/
#ifdef HAVE_PYGAME_SDL_VIDEO
int PGFT_Render_ExistingSurface(FreeTypeInstance *ft, PyFreeTypeFont *font,
    int font_size, FontRenderMode *render, PyObject *text, PySDLSurface *_surface, 
    int x, int y, PyColor *fgcolor, PyColor *bgcolor, int *_width, int *_height)
{
    static const FontRenderPtr __SDLrenderFuncs[] =
    {
        NULL,
        __render_glyph_RGB1,
        __render_glyph_RGB2,
        __render_glyph_RGB3,
        __render_glyph_RGB4
    };

    static const FontRenderPtr __MONOrenderFuncs[] =
    {
        NULL,
        __render_glyph_MONO1,
        __render_glyph_MONO2,
        __render_glyph_MONO3,
        __render_glyph_MONO4
    };

    static const FontFillPtr __RGBfillFuncs[] =
    {
        NULL,
        __fill_glyph_RGB1,
        __fill_glyph_RGB2,
        __fill_glyph_RGB3,
        __fill_glyph_RGB4
    };

    int         locked = 0;
    int         width, height;

    SDL_Surface *surface;
    FontSurface font_surf;
    FontText    *font_text;

    surface = PySDLSurface_AsSDLSurface(_surface);

    if (SDL_MUSTLOCK(surface))
    {
        if (SDL_LockSurface(surface) == -1)
        {
            _PGFT_SetError(ft, SDL_GetError (), 0);
            SDL_FreeSurface(surface);
            return -1;
        }
        locked = 1;
    }

    /* build font text */
    font_text = PGFT_LoadFontText(ft, font, font_size, render, text);

    if (!font_text)
        return -1;

    _PGFT_GetTextSize_INTERNAL(ft, font, font_size, render, font_text);

    width = PGFT_TRUNC(PGFT_ROUND(font_text->text_size.x));
    height = PGFT_TRUNC(PGFT_ROUND(font_text->text_size.y));


    /*
     * Setup target surface struct
     */
    font_surf.buffer = surface->pixels;
    font_surf.buffer_cap = ((FT_Byte *)surface->pixels) + (surface->pitch * surface->h);
    font_surf.x_offset = x;
    font_surf.y_offset = y;

    font_surf.width = surface->w;
    font_surf.height = surface->h;
    font_surf.pitch = surface->pitch / surface->format->BytesPerPixel;

    font_surf.format = surface->format;

    if (render->antialias == FONT_RENDER_ANTIALIAS)
        font_surf.render = __SDLrenderFuncs[surface->format->BytesPerPixel];
    else
        font_surf.render = __MONOrenderFuncs[surface->format->BytesPerPixel];

    font_surf.fill = __RGBfillFuncs[surface->format->BytesPerPixel];

    /* 
     * if bg color exists, paint background 
     */
    if (bgcolor)
    {
        if (bgcolor->a == 0xFF)
        {
            SDL_Rect    bg_fill; 
            FT_UInt32   fillcolor;

            fillcolor = SDL_MapRGBA(surface->format, 
                    bgcolor->r, bgcolor->g, bgcolor->b, bgcolor->a);

            bg_fill.x = (FT_Int16)x;
            bg_fill.y = (FT_Int16)y;
            bg_fill.w = (FT_UInt16)width;
            bg_fill.h = (FT_UInt16)height;

            SDL_FillRect(surface, &bg_fill, fillcolor);
        }
        else
        {
            font_surf.fill(x, y, width, height, &font_surf, bgcolor);
        }
    }

    /*
     * Render!
     */
    if (_PGFT_Render_INTERNAL(ft, font, font_text, font_size, fgcolor, &font_surf, render) != 0)
        return -1;

    *_width = width;
    *_height = height;

    if (locked)
        SDL_UnlockSurface(surface);

    return 0;
}

PyObject *PGFT_Render_NewSurface(FreeTypeInstance *ft, PyFreeTypeFont *font, 
    int ptsize, FontRenderMode *render, PyObject *text,
    PyColor *fgcolor, PyColor *bgcolor, int *_width, int *_height)
{
    int locked = 0;
    FT_UInt32 fillcolor, rmask, gmask, bmask, amask;
    SDL_Surface *surface = NULL;

    FontSurface font_surf;
    FontText *font_text;
    int width, height;

    /* build font text */
    font_text = PGFT_LoadFontText(ft, font, ptsize, render, text);

    if (!font_text)
        return NULL;

    if (_PGFT_GetTextSize_INTERNAL(ft, font, ptsize, render, font_text) != 0)
        return NULL;

    width = PGFT_TRUNC(PGFT_ROUND(font_text->text_size.x));
    height = PGFT_TRUNC(PGFT_ROUND(font_text->text_size.y));

#if SDL_BYTEORDER == SDL_BIG_ENDIAN
    rmask = 0xff000000;
    gmask = 0x00ff0000;
    bmask = 0x0000ff00;
    amask = 0x000000ff;
#else
    rmask = 0x000000ff;
    gmask = 0x0000ff00;
    bmask = 0x00ff0000;
    amask = 0xff000000;
#endif

    surface = SDL_CreateRGBSurface(SDL_SWSURFACE, 
            width, height,
            32, rmask, gmask, bmask, amask);

    if (!surface)
    {
        _PGFT_SetError(ft, SDL_GetError (), 0);
        return NULL;
    }

    if (SDL_MUSTLOCK(surface))
    {
        if (SDL_LockSurface(surface) == -1)
        {
            _PGFT_SetError(ft, SDL_GetError (), 0);
            SDL_FreeSurface(surface);
            return NULL;
        }
        locked = 1;
    }

    font_surf.buffer = surface->pixels;
    font_surf.buffer_cap = ((FT_Byte *)surface->pixels) + (surface->pitch * surface->h);
    font_surf.x_offset = font_surf.y_offset = 0;

    font_surf.width = surface->w;
    font_surf.height = surface->h;
    font_surf.pitch = surface->pitch / sizeof(FT_UInt32);

    font_surf.format = surface->format;
    font_surf.render = __render_glyph_RGB4;
    font_surf.fill = __fill_glyph_RGB4;

    /*
     * Fill our texture with the required bg color
     */
    if (bgcolor)
    {
        fillcolor = SDL_MapRGBA(surface->format, 
                bgcolor->r, bgcolor->g, bgcolor->b, bgcolor->a);
    }
    else
    {
        fillcolor = SDL_MapRGBA(surface->format, 0, 0, 0, 0);
    }

    SDL_FillRect(surface, NULL, fillcolor);

    /*
     * Render the text!
     */
    if (_PGFT_Render_INTERNAL(ft, font, font_text, ptsize, fgcolor, &font_surf, render) != 0)
    {
        SDL_FreeSurface(surface);
        return NULL;
    }

    *_width = width;
    *_height = height;

    if (locked)
        SDL_UnlockSurface(surface);

    return PySDLSurface_NewFromSDLSurface(surface);
}
#endif




/*********************************************************
 *
 * Rendering on generic arrays
 *
 *********************************************************/

PyObject *PGFT_Render_PixelArray(FreeTypeInstance *ft, PyFreeTypeFont *font,
    int ptsize, PyObject *text, int *_width, int *_height)
{
    FT_Byte *buffer = NULL;
    PyObject *array = NULL;
    FontSurface surf;
    int width, height;

    FontText *font_text;
    FontRenderMode render;
    int array_size;

    /* TODO: pixel arrays must also take custom rendering styles */
    PGFT_BuildRenderMode(&render, FT_STYLE_NORMAL,
            FONT_RENDER_HORIZONTAL, FONT_RENDER_ANTIALIAS, 0);

    /* build font text */
    font_text = PGFT_LoadFontText(ft, font, ptsize, &render, text);

    if (!font_text)
        goto cleanup;

    if (_PGFT_GetTextSize_INTERNAL(ft, font, ptsize, &render, font_text) != 0)
        goto cleanup;

    width = PGFT_TRUNC(PGFT_ROUND(font_text->text_size.x));
    height = PGFT_TRUNC(PGFT_ROUND(font_text->text_size.y));
    array_size = width * height;

    buffer = malloc((size_t)array_size);
    if (!buffer)
    {
        _PGFT_SetError(ft, "Could not allocate memory", 0);
        goto cleanup;
    }

    memset(buffer, 0xFF, (size_t)array_size);

    surf.buffer = buffer;
    surf.width = surf.pitch = width;
    surf.height = height;

    surf.format = NULL;
    surf.render = __render_glyph_ByteArray;

    if (_PGFT_Render_INTERNAL(ft, font, font_text, ptsize, 0x0, &surf, &render) != 0)
        goto cleanup;

    *_width = width;
    *_height = height;

    array = Bytes_FromStringAndSize((char *)buffer, array_size);

cleanup:
    if (buffer)
        free(buffer);

    return array;
}




/*********************************************************
 *
 * New rendering algorithm (rotation + veritical drawing)
 *
 *********************************************************/
int _PGFT_Render_INTERNAL(FreeTypeInstance *ft, PyFreeTypeFont *font, 
    FontText *text, int font_size, PyColor *fg_color,
    FontSurface *surface, FontRenderMode *render)
{
    const FT_Fixed center = (1 << 15); // 0.5

    int         n;
    FT_Vector   pen, advances[MAX_GLYPHS];
    FT_Face     face;
    FT_Error    error;

    FT_Fixed    bold_str = 0;

    int         x = (surface->x_offset << 6);
    int         y = (surface->y_offset << 6);

    assert(text->text_size.x);
    assert(text->text_size.y);

    x += (text->text_size.x / 2);
    y += (text->text_size.y / 2);
    y -= (text->baseline_offset.y);

    /******************************************************
     * Load scaler, size & face
     ******************************************************/
    face = _PGFT_GetFaceSized(ft, font, font_size);

    if (!face)
    {
        _PGFT_SetError(ft, "Failed to resize face", 0);
        return -1;
    }

    /******************************************************
     * Load advance information
     ******************************************************/

    error = PGFT_GetTextAdvances(ft, font, font_size, render, text, advances);

    if (error)
    {
        _PGFT_SetError(ft, "Failed to load advance glyph advances", error);
        return error;
    }

    /******************************************************
     * Prepare pen for drawing 
     ******************************************************/

    /* change to Cartesian coordinates */
    y = (surface->height << 6) - y;

    /* get the extent, which we store in the last slot */
    pen = advances[text->length - 1];

    pen.x = FT_MulFix(pen.x, center); 
    pen.y = FT_MulFix(pen.y, center);

    if (!render->vertical)
        pen.y += PGFT_ROUND(text->glyph_size.y / 2);

    /* get pen position */
    if (render->matrix && FT_IS_SCALABLE(face))
    {
        FT_Vector_Transform(&pen, render->matrix);
        pen.x = x - pen.x;
        pen.y = y - pen.y;
    }
    else
    {
        pen.x = PGFT_ROUND(x - pen.x);
        pen.y = PGFT_ROUND(y - pen.y);
    }

    /******************************************************
     * Prepare data for glyph transformations
     ******************************************************/
    if (render->style & FT_STYLE_BOLD)
    {
        bold_str = PGFT_GetBoldStrength(face);
    }

    /******************************************************
     * Draw text
     ******************************************************/

    for (n = 0; n < text->length; ++n)
    {
        FT_Glyph image;
        FT_BBox bbox;

        FontGlyph *glyph = &(text->glyphs[n]);

        if (!glyph->image)
            continue;

        /* copy image */
        error = FT_Glyph_Copy(glyph->image, &image);
        if (error)
            continue;

        if (image->format == FT_GLYPH_FORMAT_OUTLINE)
        {
            FT_OutlineGlyph outline;
            outline = (FT_OutlineGlyph)image;

            if (render->style & FT_STYLE_BOLD)
            {
                FT_Outline_Embolden(&(outline->outline), bold_str);
            }

            if (render->style & FT_STYLE_ITALIC)
            {
                FT_Outline_Transform(&(outline->outline), &PGFT_SlantMatrix);
            }

            if (render->vertical)
                error = FT_Glyph_Transform(image, NULL, &glyph->vvector);

            if (!error)
                error = FT_Glyph_Transform(image, render->matrix, &pen);

            if (error)
            {
                FT_Done_Glyph(image);
                continue;
            }
        }
        else
        {
            FT_BitmapGlyph  bitmap = (FT_BitmapGlyph)image;

            if (render->vertical)
            {
                bitmap->left += (glyph->vvector.x + pen.x) >> 6;
                bitmap->top  += (glyph->vvector.x + pen.y) >> 6;
            }
            else
            {
                bitmap->left += pen.x >> 6;
                bitmap->top  += pen.y >> 6;
            }
        }

        if (render->matrix)
            FT_Vector_Transform(advances + n, render->matrix);

        pen.x += advances[n].x;
        pen.y += advances[n].y;

        FT_Glyph_Get_CBox(image, FT_GLYPH_BBOX_PIXELS, &bbox);

        if (bbox.xMax > 0 && bbox.yMax > 0 &&
            bbox.xMin < surface->width &&
            bbox.yMin < surface->height)
        {
            int         left, top;
            FT_Bitmap*  source;
            FT_BitmapGlyph  bitmap;

            if (image->format == FT_GLYPH_FORMAT_OUTLINE)
            {
                FT_Render_Mode render_mode = 
                    render->antialias ?
                    FT_RENDER_MODE_NORMAL :
                    FT_RENDER_MODE_MONO;

                /* render the glyph to a bitmap, don't destroy original */
                FT_Glyph_To_Bitmap(&image, render_mode, NULL, 0);
            }

            if (image->format != FT_GLYPH_FORMAT_BITMAP)
                continue;

            bitmap = (FT_BitmapGlyph)image;
            source = &bitmap->bitmap;

            left = bitmap->left;
            top = bitmap->top;

            top = surface->height - top;

            surface->render(left, top, surface, source, fg_color);
        }

        FT_Done_Glyph(image);
    } /* END OF RENDERING LOOP */

    if (render->style & FT_STYLE_UNDERLINE)
    {
        surface->fill(surface->x_offset, surface->y_offset + text->baseline_offset.y + text->underline_pos, 
                text->text_size.x >> 6, text->underline_h, 
                surface, fg_color);
    }

    return error;
}
