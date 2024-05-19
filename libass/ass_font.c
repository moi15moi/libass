/*
 * Copyright (C) 2006 Evgeniy Stepanov <eugeni.stepanov@gmail.com>
 *
 * This file is part of libass.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"
#include "ass_compat.h"

#include <inttypes.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H
#include FT_GLYPH_H
#include FT_TRUETYPE_TABLES_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_IDS_H
#include FT_TYPE1_TABLES_H
#include <limits.h>

#include "ass.h"
#include "ass_library.h"
#include "ass_font.h"
#include "ass_fontselect.h"
#include "ass_utils.h"
#include "ass_shaper.h"

#if FREETYPE_MAJOR == 2 && FREETYPE_MINOR < 6
// The lowercase name is still included (as a macro) but deprecated as of 2.6, so avoid using it if we can
#define FT_SFNT_OS2 ft_sfnt_os2
#endif

/**
 *  Get the mbcs codepoint from the output bytes of iconv/WideCharToMultiByte,
 *  by treating the bytes as a prefix-zero-byte-omitted big-endian integer.
 */
static inline uint32_t pack_mbcs_bytes(const char *bytes, size_t length)
{
    uint32_t ret = 0;
    for (size_t i = 0; i < length; ++i) {
        ret <<= 8;
        ret |= (uint8_t) bytes[i];
    }
    return ret;
}

/**
 * Convert a UCS-4 code unit to a packed uint32_t in given multibyte encoding.
 *
 * We don't exclude Cygwin for Windows since we use WideCharToMultiByte only,
 * this shall not violate any Cygwin restrictions on Windows APIs.
 */
#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static uint32_t convert_unicode_to_mb(FT_Encoding encoding, uint32_t codepoint)
{
    // map freetype encoding to windows codepage
    UINT codepage;
    switch (encoding) {
    case FT_ENCODING_MS_SJIS:
        codepage = 932;
        break;
    case FT_ENCODING_MS_GB2312:
        codepage = 936;
        break;
    case FT_ENCODING_MS_BIG5:
        codepage = 950;
        break;
    case FT_ENCODING_MS_WANSUNG:
        codepage = 949;
        break;
    case FT_ENCODING_MS_JOHAB:
        codepage = 1361;
        break;
    default:
        return 0;
    }

    WCHAR input_buffer[2];
    size_t inbuf_size;

    // encode surrogate pair, assuming codepoint > 0 && codepoint <= 10FFFF
    if (codepoint >= 0x10000) {
        // surrogate pair required
        inbuf_size = 2;
        input_buffer[0] = 0xD7C0 + (codepoint >> 10);
        input_buffer[1] = 0xDC00 + (codepoint & 0x3FF);
    } else {
        inbuf_size = 1;
        input_buffer[0] = codepoint;
    }

    // do convert
    char output_buffer[2];
    BOOL conversion_fail;
    int output_length = WideCharToMultiByte(codepage, WC_NO_BEST_FIT_CHARS, input_buffer, inbuf_size,
                                          output_buffer, sizeof(output_buffer), NULL, &conversion_fail);
    if (output_length == 0 || conversion_fail)
        return 0;

    return pack_mbcs_bytes(output_buffer, output_length);
}
#elif defined(CONFIG_ICONV)

#include <iconv.h>

static uint32_t convert_unicode_to_mb(FT_Encoding encoding, uint32_t codepoint)
{
    typedef struct { const char *names[5]; } EncodingList;

    EncodingList encoding_list;

    // map freetype encoding to iconv encoding
    switch (encoding) {
    case FT_ENCODING_MS_SJIS:
        encoding_list = (EncodingList) {{"CP932", "SHIFT_JIS", NULL}};
        break;
    case FT_ENCODING_MS_GB2312:
        encoding_list = (EncodingList) {{"CP936", "GBK", "GB18030", "GB2312", NULL}};
        break;
    case FT_ENCODING_MS_BIG5:
        encoding_list = (EncodingList) {{"CP950", "BIG5", NULL}};
        break;
    case FT_ENCODING_MS_WANSUNG:
        encoding_list = (EncodingList) {{"CP949", "EUC-KR", NULL}};
        break;
    case FT_ENCODING_MS_JOHAB:
        encoding_list = (EncodingList) {{"CP1361", "JOHAB", NULL}};
        break;
    default:
        return 0;
    }

    // open iconv context
    const char **encoding_str = encoding_list.names;
    iconv_t cd = (iconv_t) -1;
    while (*encoding_str) {
        cd = iconv_open(*encoding_str, "UTF-32LE");
        if (cd != (iconv_t) -1) break;
        ++encoding_str;
    }
    if (cd == (iconv_t) -1)
        return 0;

    char input_buffer[4];
    char output_buffer[2]; // MS-flavour encodings only need 2 bytes
    uint32_t result = codepoint;

    // convert input codepoint to little endian uint32_t bytearray,
    // result becomes 0 after the loop finishes
    for (int i = 0; i < 4; ++i) {
        input_buffer[i] = result & 0xFF;
        result >>= 8;
    }

    // do actual convert, only reversible converts are valid, since we are converting unicode to something else
    size_t inbuf_size = sizeof(input_buffer);
    size_t outbuf_size = sizeof(output_buffer);
    char *inbuf = input_buffer;
    char *outbuf = output_buffer;
    if (iconv(cd, &inbuf, &inbuf_size, &outbuf, &outbuf_size))
        goto clean;

    // now we have multibyte string in output_buffer
    // assemble those bytes into uint32_t
    size_t output_length = sizeof(output_buffer) - outbuf_size;
    result = pack_mbcs_bytes(output_buffer, output_length);

clean:
    iconv_close(cd);
    return result;
}
#else
static uint32_t convert_unicode_to_mb(FT_Encoding encoding, uint32_t codepoint) {
    // just a stub
    // we can't handle this cmap, fallback
    return 0;
}
#endif

/**
 * Select a good charmap, prefer Microsoft Unicode charmaps.
 * Otherwise, let FreeType decide.
 */
void ass_charmap_magic(ASS_Library *library, FT_Face face)
{
    int i;
    int ms_cmap = -1, ms_unicode_cmap = -1;

    // Search for a Microsoft Unicode cmap
    for (i = 0; i < face->num_charmaps; ++i) {
        FT_CharMap cmap = face->charmaps[i];
        unsigned pid = cmap->platform_id;
        unsigned eid = cmap->encoding_id;
        if (pid == TT_PLATFORM_MICROSOFT) {
            switch (eid) {
            case TT_MS_ID_UCS_4:
                // Full Unicode cmap: select this immediately
                FT_Set_Charmap(face, cmap);
                return;
            case TT_MS_ID_UNICODE_CS:
                // BMP-only Unicode cmap: select this
                // if no fuller Unicode cmap exists
                if (ms_unicode_cmap < 0)
                    ms_unicode_cmap = ms_cmap = i;
                break;
            default:
                // Non-Unicode cmap: select this if no Unicode cmap exists
                if (ms_cmap < 0)
                    ms_cmap = i;
            }
        }
    }

    // Try the first Microsoft BMP cmap if no MS cmap had full Unicode,
    // or the first MS cmap of any kind if none of them had Unicode at all
    if (ms_cmap >= 0) {
        FT_CharMap cmap = face->charmaps[ms_cmap];
        FT_Set_Charmap(face, cmap);
        return;
    }

    if (!face->charmap) {
        if (face->num_charmaps == 0) {
            ass_msg(library, MSGL_WARN, "Font face with no charmaps");
            return;
        }
        ass_msg(library, MSGL_WARN,
                "No charmap autodetected, trying the first one");
        FT_Set_Charmap(face, face->charmaps[0]);
        return;
    }
}

/**
 * Adjust char index if the charmap is weird
 * (currently all non-Unicode Microsoft cmap)
 */

uint32_t ass_font_index_magic(FT_Face face, uint32_t symbol)
{
    if (!face->charmap)
        return symbol;

    if (face->charmap->platform_id == TT_PLATFORM_MICROSOFT) {
        switch (face->charmap->encoding) {
        case FT_ENCODING_MS_SYMBOL:
            return 0xF000 | symbol;
        case FT_ENCODING_MS_SJIS:
        case FT_ENCODING_MS_GB2312:
        case FT_ENCODING_MS_BIG5:
        case FT_ENCODING_MS_WANSUNG:
        case FT_ENCODING_MS_JOHAB:
            return convert_unicode_to_mb(face->charmap->encoding, symbol);
        default:
            return symbol;
        }
    }

    return symbol;
}

static void set_font_metrics(FT_Face face)
{
    // Mimicking GDI's behavior for asc/desc/height.
    // These fields are (apparently) sometimes used for signed values,
    // despite being unsigned in the spec.
    TT_OS2 *os2 = FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
    if (os2 && ((short)os2->usWinAscent + (short)os2->usWinDescent != 0)) {
        face->ascender  =  (short)os2->usWinAscent;
        face->descender = -(short)os2->usWinDescent;
        face->height    = face->ascender - face->descender;
    }

    // If we didn't have usable Win values in the OS/2 table,
    // then the values from FreeType will still be in these fields.
    // It'll use either the OS/2 typo metrics or the hhea ones.
    // If the font has typo metrics but FreeType didn't use them
    // (either old FT or USE_TYPO_METRICS not set), we'll try those.
    // In the case of a very broken font that has none of those options,
    // we fall back on using face.bbox.
    // Anything without valid OS/2 Win values isn't supported by VSFilter,
    // so at this point compatibility's out the window and we're just
    // trying to render _something_ readable.
    if (face->ascender - face->descender == 0 || face->height == 0) {
        if (os2 && (os2->sTypoAscender - os2->sTypoDescender) != 0) {
            face->ascender = os2->sTypoAscender;
            face->descender = os2->sTypoDescender;
            face->height = face->ascender - face->descender;
        } else {
            face->ascender = face->bbox.yMax;
            face->descender = face->bbox.yMin;
            face->height = face->ascender - face->descender;
        }
    }
}

FT_Face ass_face_open(ASS_Library *lib, FT_Library ftlib, const char *path,
                      const char *postscript_name, int index)
{
    FT_Face face;
    int error = FT_New_Face(ftlib, path, index, &face);
    if (error) {
        ass_msg(lib, MSGL_WARN, "Error opening font: '%s', %d", path, index);
        return NULL;
    }

    if (index >= 0) {
        return face;
    } else {
        // The font provider gave us a postscript name and is not sure
        // about the face index.. so use the postscript name to find the
        // correct face_index in the collection!
        for (int i = 0; i < face->num_faces; i++) {
            FT_Done_Face(face);
            error = FT_New_Face(ftlib, path, i, &face);
            if (error) {
                ass_msg(lib, MSGL_WARN, "Error opening font: '%s', %d", path, i);
                return NULL;
            }

            // If there is only one face, don't bother checking the name.
            // The font might not even *have* a valid PostScript name.
            if (!i && face->num_faces == 1)
                return face;

            // Otherwise, we really need a name to search for.
            if (!postscript_name) {
                FT_Done_Face(face);
                return NULL;
            }

            const char *face_psname = FT_Get_Postscript_Name(face);
            if (face_psname != NULL &&
                strcmp(face_psname, postscript_name) == 0)
                return face;
        }

        FT_Done_Face(face);
        ass_msg(lib, MSGL_WARN, "Failed to find font '%s' in file: '%s'",
                postscript_name, path);
        return NULL;
    }
}

static unsigned long
read_stream_font(FT_Stream stream, unsigned long offset, unsigned char *buffer,
                 unsigned long count)
{
    ASS_FontStream *font = (ASS_FontStream *)stream->descriptor.pointer;

    font->func(font->priv, buffer, offset, count);
    return count;
}

static void
close_stream_font(FT_Stream stream)
{
    free(stream->descriptor.pointer);
    free(stream);
}

FT_Face ass_face_stream(ASS_Library *lib, FT_Library ftlib, const char *name,
                        const ASS_FontStream *stream, int index)
{
    ASS_FontStream *fs = calloc(1, sizeof(ASS_FontStream));
    if (!fs)
        return NULL;
    *fs = *stream;

    FT_Stream ftstream = calloc(1, sizeof(FT_StreamRec));
    if (!ftstream) {
        free(fs);
        return NULL;
    }
    ftstream->size  = stream->func(stream->priv, NULL, 0, 0);
    ftstream->read  = read_stream_font;
    ftstream->close = close_stream_font;
    ftstream->descriptor.pointer = (void *)fs;

    FT_Open_Args args = {
        .flags  = FT_OPEN_STREAM,
        .stream = ftstream,
    };

    FT_Face face;
    int error = FT_Open_Face(ftlib, &args, index, &face);
    if (error) {
        if (name) {
            ass_msg(lib, MSGL_WARN,
                    "Error opening memory font: '%s'", name);
        } else {
            ass_msg(lib, MSGL_WARN,
                    "Error opening memory font");
        }
        return NULL;
    }

    return face;
}

/**
 * \brief Select a face with the given charcode and add it to ASS_Font
 * \return index of the new face in font->faces, -1 if failed
 */
static int add_face(ASS_FontSelector *fontsel, ASS_Font *font, uint32_t ch)
{
    char *path;
    char *postscript_name = NULL;
    int i, index, uid;
    ASS_FontStream stream = { NULL, NULL };
    FT_Face face;
    int ret = -1;

    if (font->n_faces == ASS_FONT_MAX_FACES)
        return -1;

    path = ass_font_select(fontsel, font, &index,
            &postscript_name, &uid, &stream, ch);

    if (!path)
        return -1;

    for (i = 0; i < font->n_faces; i++) {
        if (font->faces_uid[i] == uid) {
            ass_msg(font->library, MSGL_INFO,
                    "Got a font face that already is available! Skipping.");
            return i;
        }
    }

    if (stream.func) {
        face = ass_face_stream(font->library, font->ftlibrary, path,
                               &stream, index);
    } else {
        face = ass_face_open(font->library, font->ftlibrary, path,
                             postscript_name, index);
    }

    if (!face)
        return -1;

    ass_charmap_magic(font->library, face);
    set_font_metrics(face);

    font->faces[font->n_faces] = face;
    font->faces_uid[font->n_faces] = uid;
    if (!ass_create_hb_font(font, font->n_faces)) {
        FT_Done_Face(face);
        goto fail;
    }

    ret = font->n_faces++;

fail:
    return ret;
}

/**
 * \brief Create a new ASS_Font according to "desc" argument
 */
ASS_Font *ass_font_new(ASS_Renderer *render_priv, ASS_FontDesc *desc)
{
    ASS_Font *font = ass_cache_get(render_priv->cache.font_cache, desc, render_priv);
    if (!font)
        return NULL;
    if (font->library)
        return font;
    ass_cache_dec_ref(font);
    return NULL;
}

size_t ass_font_construct(void *key, void *value, void *priv)
{
    ASS_Renderer *render_priv = priv;
    ASS_FontDesc *desc = key;
    ASS_Font *font = value;

    font->library = render_priv->library;
    font->ftlibrary = render_priv->ftlibrary;
    font->n_faces = 0;
    font->desc.family = desc->family;
    font->desc.bold = desc->bold;
    font->desc.italic = desc->italic;
    font->desc.charset = desc->charset;
    font->desc.vertical = desc->vertical;

    int error = add_face(render_priv->fontselect, font, 0);
    if (error == -1)
        font->library = NULL;
    return 1;
}

void ass_face_set_size(FT_Face face, double size)
{
    FT_Size_RequestRec rq;
    memset(&rq, 0, sizeof(rq));
    rq.type = FT_SIZE_REQUEST_TYPE_REAL_DIM;
    rq.width = 0;
    rq.height = double_to_d6(size);
    rq.horiResolution = rq.vertResolution = 0;
    FT_Request_Size(face, &rq);
}

bool ass_face_is_postscript(FT_Face face)
{
    PS_FontInfoRec postscript_info;
    return !FT_Get_PS_Font_Info(face, &postscript_info);
}

/**
 * \brief Get face weight
 **/
int ass_face_get_weight(FT_Face face)
{
    TT_OS2 *os2 = FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
    FT_UShort os2Weight = os2 ? os2->usWeightClass : 0;
    switch (os2Weight) {
    case 0:
        return 300 * !!(face->style_flags & FT_STYLE_FLAG_BOLD) + 400;
    case 1:
        return 100;
    case 2:
        return 200;
    case 3:
        return 300;
    case 4:
        return 350;
    case 5:
        return 400;
    case 6:
        return 600;
    case 7:
        return 700;
    case 8:
        return 800;
    case 9:
        return 900;
    default:
        return os2Weight;
    }
}

static FT_Long fsSelection_to_style_flags(uint16_t fsSelection)
{
    FT_Long ret = 0;

    if (fsSelection & 1)
        ret |= FT_STYLE_FLAG_ITALIC;
    if (fsSelection & (1 << 5))
        ret |= FT_STYLE_FLAG_BOLD;

    return ret;
}

static FT_Long macStyle_to_style_flags(uint16_t macStyle)
{
    FT_Long ret = 0;

    if (macStyle & (1 << 1))
        ret |= FT_STYLE_FLAG_ITALIC;
    if (macStyle & (1 << 0))
        ret |= FT_STYLE_FLAG_BOLD;

    return ret;
}

FT_Long ass_face_get_style_flags(FT_Face face)
{
    // If we have an OS/2 table, compute this ourselves, since FreeType
    // will mix in some flags that GDI ignores.
    TT_OS2 *os2 = FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
    if (os2)
        return fsSelection_to_style_flags(os2->fsSelection);

    TT_Header *head = FT_Get_Sfnt_Table(face, FT_SFNT_HEAD);
    if (head)
        return macStyle_to_style_flags(head->Mac_Style);

    return face->style_flags;
}

/**
 * \brief Is the charset is from a Double-Byte Character Set
 **/
bool ass_face_is_charset_dbcs(int charset)
{
    return (charset == ASS_SHIFTJIS_CHARSET ||
        charset == ASS_HANGUL_CHARSET ||
        charset == ASS_CHINESEBIG5_CHARSET ||
        charset == ASS_GB2312_CHARSET);
}
/**
 * \brief Is the charset is from a Double-Byte Character Set
 **/
bool ass_face_is_any_charset_dbcs(int *supported_charsets, size_t num_supported_charset, ASS_CharacterSet guessed_charset) {

    if (ass_face_is_charset_dbcs(guessed_charset))
        return true;

    for (size_t i = 0; i < num_supported_charset; i++) {
        if (ass_face_is_charset_dbcs(supported_charsets[i]))
            return true;
    }
    return false;    
}

bool ass_face_has_japanese_signature(FT_Face face, FT_ULong ulCodePageRange1)
{
    // GDI also has a special case if the flag ASS_FS_CHINESESIMP is set in ulCodePageRange1 and
    // the unique face name is one of these names:
    // - Microsoft:MS Mincho:1995
    // - Microsoft:MS PMincho:1995
    // - Microsoft:MS Gothic:1995
    // - Microsoft:MS PGothic:1995
    return (
        !(ulCodePageRange1 & ASS_FS_JISJAPAN) &&
        face->charmap->platform_id == TT_PLATFORM_MICROSOFT &&
        face->charmap->encoding_id == TT_MS_ID_SJIS &&
        ass_font_index_magic(face, HALFWIDTH_KATAKANA_LETTER_A) &&
        ass_font_index_magic(face, HALFWIDTH_KATAKANA_LETTER_I) &&
        ass_font_index_magic(face, HALFWIDTH_KATAKANA_LETTER_U) &&
        ass_font_index_magic(face, HALFWIDTH_KATAKANA_LETTER_E) &&
        ass_font_index_magic(face, HALFWIDTH_KATAKANA_LETTER_O)
    );
}

ASS_CmapType ass_face_get_cmap_type(FT_Face face)
{
    size_t cmap_type = ASS_CMAP_UNKNOWN;
    FT_Long cmap_format = FT_Get_CMap_Format(face->charmap);
    TT_OS2 *os2;
    TT_Postscript *post;

    if (face->charmap->platform_id == TT_PLATFORM_MICROSOFT) {
        if (cmap_format == 4) {
            if (face->charmap->encoding_id == TT_MS_ID_SJIS ||
                face->charmap->encoding_id == TT_MS_ID_PRC ||
                face->charmap->encoding_id == TT_MS_ID_BIG_5 ||
                face->charmap->encoding_id == TT_MS_ID_WANSUNG) {
                cmap_type = ASS_CMAP_NOT_UNICODE;
            } else {
                cmap_type = ASS_CMAP_UNICODE;
            }
        } else if (cmap_format == 2) {
            if (face->charmap->encoding_id == TT_MS_ID_SJIS ||
                face->charmap->encoding_id == TT_MS_ID_PRC ||
                face->charmap->encoding_id == TT_MS_ID_BIG_5 ||
                face->charmap->encoding_id == TT_MS_ID_WANSUNG)
                cmap_type = ASS_CMAP_HIGH_BYTE;
        }
        
        if (cmap_type == ASS_CMAP_UNKNOWN)
            return cmap_type;

        if (face->charmap->encoding_id == TT_MS_ID_SYMBOL_CS) {
            os2 = FT_Get_Sfnt_Table(face, FT_SFNT_OS2);

            if (os2) {
                if (os2->fsSelection & 0xFF == ASS_ANSI_CHARSET &&
                    os2->panose[ASS_bFamilyType] == ASS_PAN_FAMILY_PICTORIAL)
                    cmap_type = ASS_CMAP_SYMBOL;
            }
        }

        // When the platform enc id is 0 or 1, GDI actually do some check to see if the font is a symbol
        // or if it is a old BiDi font
    }

    if (face->charmap->platform_id == TT_PLATFORM_MACINTOSH &&
        face->charmap->encoding_id == TT_MAC_ID_ROMAN &&
        cmap_format == 0) {
        post = FT_Get_Sfnt_Table(face, FT_SFNT_POST);

        // 0x20000 means version 2.0
        if (post->FormatType == 0x20000) {
            // GDI performs a check with the index of the glyph name available in the post table,
            // but since FreeType reorders the glyph indices,
            // it isn't possible to make this check without manually parsing
            // the post table with FT_Load_Sfnt_Table.
            // TODO: Parse the post table manually to perform this check.
            cmap_type = ASS_CMAP_MAC_ROMAN;
        } else {
            cmap_type = ASS_CMAP_WIN_ANSI;
        }
    }

    return cmap_type;   
}

/**
 * \note  Be sure that the panose array has a length of at least 10 bytes.
 * \brief Sets the Panose data.
 **/
void ass_face_set_panose(FT_Face face, FT_Long style_flags, FT_Byte *panose) {
    TT_OS2 *os2 = FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
    if (os2) {
        memcpy(panose, os2->panose, 10 * sizeof(FT_Byte));
        return;
    }

    bool is_fixed_pitch = false;
    TT_Postscript *post = FT_Get_Sfnt_Table(face, FT_SFNT_POST);
    if (post)
        is_fixed_pitch = !!post->isFixedPitch;
    memset(panose, 0, 10 * sizeof(FT_Byte));
    panose[ASS_bFamilyType] = ASS_PAN_FAMILY_TEXT_DISPLAY;
    panose[ASS_bWeight] = (style_flags & FT_STYLE_FLAG_BOLD) ? ASS_PAN_WEIGHT_BOLD : ASS_PAN_WEIGHT_BOOK;
    panose[ASS_bProportion] = is_fixed_pitch ? ASS_PAN_PROP_MONOSPACED : ASS_PAN_ANY;
}

bool ass_face_has_os2_version_1_or_higher(FT_Face face) {
    TT_OS2 *os2 = FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
    if (os2)
        return !!os2->version;
    return false;
}

size_t ass_face_set_charsets_from_ulCodePageRange1(FT_ULong ulCodePageRange1, enum ASS_CharacterSet *supported_charsets,
                                                   bool is_postscript) {
    size_t num_charsets = 0;

    if (ulCodePageRange1 & ASS_FS_LATIN1) 
        supported_charsets[num_charsets++] = ASS_ANSI_CHARSET;
    if (ulCodePageRange1 & ASS_FS_LATIN2) 
        supported_charsets[num_charsets++] = ASS_EASTEUROPE_CHARSET;
    if (ulCodePageRange1 & ASS_FS_CYRILLIC) 
        supported_charsets[num_charsets++] = ASS_RUSSIAN_CHARSET;
    if (ulCodePageRange1 & ASS_FS_GREEK) 
        supported_charsets[num_charsets++] = ASS_GREEK_CHARSET;
    if (ulCodePageRange1 & ASS_FS_TURKISH) 
        supported_charsets[num_charsets++] = ASS_TURKISH_CHARSET;
    if (ulCodePageRange1 & ASS_FS_HEBREW) 
        supported_charsets[num_charsets++] = ASS_HEBREW_CHARSET;
    if (ulCodePageRange1 & ASS_FS_ARABIC) 
        supported_charsets[num_charsets++] = ASS_ARABIC_CHARSET;
    if (ulCodePageRange1 & ASS_FS_BALTIC) 
        supported_charsets[num_charsets++] = ASS_BALTIC_CHARSET;
    if (ulCodePageRange1 & ASS_FS_VIETNAMESE) 
        supported_charsets[num_charsets++] = ASS_VIETNAMESE_CHARSET;
    if (ulCodePageRange1 & ASS_FS_THAI) 
        supported_charsets[num_charsets++] = ASS_THAI_CHARSET;
    if (ulCodePageRange1 & ASS_FS_JISJAPAN) 
        supported_charsets[num_charsets++] = ASS_SHIFTJIS_CHARSET;
    if (ulCodePageRange1 & ASS_FS_CHINESESIMP) 
        supported_charsets[num_charsets++] = ASS_GB2312_CHARSET;
    if (ulCodePageRange1 & ASS_FS_WANSUNG) 
        supported_charsets[num_charsets++] = ASS_HANGUL_CHARSET;
    if (ulCodePageRange1 & ASS_FS_CHINESETRAD) 
        supported_charsets[num_charsets++] = ASS_CHINESEBIG5_CHARSET;
    if (ulCodePageRange1 & ASS_FS_JOHAB) 
        supported_charsets[num_charsets++] = ASS_JOHAB_CHARSET;
    if (ulCodePageRange1 & ASS_FS_SYMBOL)
        supported_charsets[num_charsets++] = ASS_SYMBOL_CHARSET;
    if (is_postscript && (ulCodePageRange1 & ASS_FS_MAC))
        supported_charsets[num_charsets++] = ASS_MAC_CHARSET;

    return num_charsets;
}

ASS_CharacterSet ass_face_get_guessed_charset(FT_Face face, FT_Byte* panose)
{
    if (!panose)
        return ASS_DEFAULT_CHARSET;

    TT_OS2 *os2 = FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
    size_t cmap_type = ass_face_get_cmap_type(face);

    
    uint8_t charset_from_fsSelection = 0;
    if (os2)
        // For old font
        charset_from_fsSelection = os2->fsSelection >> 8;
    uint8_t guessed_charset = ASS_ANSI_CHARSET;

    if (
        os2 &&
        os2->version > 0 &&
        os2->ulCodePageRange1 &&
        !ass_face_has_japanese_signature(face, os2->ulCodePageRange1)
    ) {
        FT_ULong ulCodePageRange1 = os2->ulCodePageRange1;

        if (ulCodePageRange1 & ( ASS_FS_JISJAPAN | ASS_FS_CHINESETRAD | ASS_FS_CHINESESIMP | ASS_FS_WANSUNG)) {
            // GDI also calls [GetACP](https://learn.microsoft.com/en-us/windows/win32/api/winnls/nf-winnls-getacp),
            // translate the ANSI code page to a charset and it compare it to ulCodePageRange1
            if (ulCodePageRange1 & ASS_FS_JISJAPAN)
                guessed_charset = ASS_SHIFTJIS_CHARSET ;                   
            else if (ulCodePageRange1 & ASS_FS_CHINESETRAD)
                guessed_charset = ASS_CHINESEBIG5_CHARSET;
            else if (ulCodePageRange1 & ASS_FS_CHINESESIMP)
                guessed_charset = ASS_GB2312_CHARSET;
            else
                guessed_charset = ASS_HANGUL_CHARSET;
        } else {
            guessed_charset = charset_from_fsSelection;

            if (
                !charset_from_fsSelection &&
                panose[ASS_bFamilyType] == ASS_PAN_FAMILY_PICTORIAL &&
                cmap_type == ASS_CMAP_SYMBOL
            )
                guessed_charset = ASS_SYMBOL_CHARSET;
        }
    } else {
        if (cmap_type == ASS_CMAP_HIGH_BYTE) {
            if (face->charmap->encoding_id == TT_MS_ID_SJIS)
                guessed_charset = ASS_SHIFTJIS_CHARSET;
            else if (face->charmap->encoding_id == TT_MS_ID_PRC)
                guessed_charset = ASS_GB2312_CHARSET;
            else if (face->charmap->encoding_id == TT_MS_ID_BIG_5)
                guessed_charset = ASS_CHINESEBIG5_CHARSET;
            else if (face->charmap->encoding_id == TT_MS_ID_WANSUNG)
                guessed_charset = ASS_HANGUL_CHARSET;
            else
                guessed_charset = ASS_ANSI_CHARSET;
        } else if (
            ass_font_index_magic(face, HALFWIDTH_KATAKANA_LETTER_A) &&
            ass_font_index_magic(face, HALFWIDTH_KATAKANA_LETTER_I) &&
            ass_font_index_magic(face, HALFWIDTH_KATAKANA_LETTER_U) &&
            ass_font_index_magic(face, HALFWIDTH_KATAKANA_LETTER_E) &&
            ass_font_index_magic(face, HALFWIDTH_KATAKANA_LETTER_O)
        ) {
            guessed_charset = ASS_SHIFTJIS_CHARSET;
        } else if (
            ass_font_index_magic(face, CJK_UNIFIED_IDEOGRAPH_61D4) &&
            ass_font_index_magic(face, CJK_UNIFIED_IDEOGRAPH_9EE2)
        ) {
            guessed_charset = ASS_GB2312_CHARSET;
        } else if (
            ass_font_index_magic(face, CJK_UNIFIED_IDEOGRAPH_9F79) &&
            ass_font_index_magic(face, CJK_UNIFIED_IDEOGRAPH_9F98)
        ) {
            guessed_charset = ASS_CHINESEBIG5_CHARSET;
        } else if (
            ass_font_index_magic(face, HANGUL_SYLLABLE_GA) &&
            ass_font_index_magic(face, HANGUL_SYLLABLE_HA)
        ) {
            guessed_charset = ASS_HANGUL_CHARSET;
        } else if (
            ass_font_index_magic(face, PRIVATE_USE_AREA_E000) &&
            (face->charmap->platform_id == TT_PLATFORM_MICROSOFT &&
            face->charmap->encoding_id >= TT_MS_ID_SJIS &&
            face->charmap->encoding_id <= TT_MS_ID_JOHAB)

        ) {
            // If a font has the char PRIVATE_USE_AREA_E000 and [GetACP](https://learn.microsoft.com/en-us/windows/win32/api/winnls/nf-winnls-getacp)
            // return 932, 936, 949, 950 or 1361, it will set the guessed_charset to the correponding charset.
            // Since it depend on the user environnement, we simply fallback on the cmap encoding id.
            if (face->charmap->encoding_id == TT_MS_ID_SJIS)
                guessed_charset = ASS_SHIFTJIS_CHARSET;
            else if (face->charmap->encoding_id == TT_MS_ID_PRC)
                guessed_charset = ASS_GB2312_CHARSET;
            else if (face->charmap->encoding_id == TT_MS_ID_BIG_5)
                guessed_charset = ASS_CHINESEBIG5_CHARSET;
            else if (face->charmap->encoding_id == TT_MS_ID_WANSUNG)
                guessed_charset = ASS_HANGUL_CHARSET;
            else
                guessed_charset = ASS_JOHAB_CHARSET;
        } else {
            guessed_charset = charset_from_fsSelection;

            if (
                !charset_from_fsSelection &&
                panose[ASS_bFamilyType] == ASS_PAN_FAMILY_PICTORIAL &&
                cmap_type == ASS_CMAP_SYMBOL
            )
                guessed_charset = ASS_SYMBOL_CHARSET;
        }
    }


    if (os2 && face->charmap->platform_id == TT_PLATFORM_MICROSOFT) {
            const size_t LAST_CHAR = 255;

            if (os2->usFirstCharIndex > LAST_CHAR && os2->usLastCharIndex > LAST_CHAR)
                guessed_charset = ASS_SYMBOL_CHARSET;
    }

    return guessed_charset;
}

/**
 * \note  Always call ass_face_has_os2_version_1_or_higher before this method.
 * \brief Get the supported charset for a postscript font.
 **/
size_t ass_face_get_charsets_postscript(FT_Face face, ASS_CharacterSet *supported_charsets)
{
    size_t num_charsets = 0;
    TT_OS2 *os2 = FT_Get_Sfnt_Table(face, FT_SFNT_OS2);

    if (!os2)
        // os2 should never be null (see note).
        return num_charsets;

    num_charsets = ass_face_set_charsets_from_ulCodePageRange1(os2->ulCodePageRange1, supported_charsets, true);
    // TODO see if it is a good idea
    if (!num_charsets)
        supported_charsets[num_charsets++] = ASS_DEFAULT_CHARSET;

    return num_charsets;    
}

/**
 * \note  Always call ass_face_has_os2_version_1_or_higher before this method.
 * \brief Get the supported charset for a truetype font.
 **/
size_t ass_face_get_charsets_truetype(FT_Face face, int *guessed_charset,
                                      FT_Byte* panose, int *supported_charsets) {
    size_t num_charsets = 0;
    *guessed_charset = ass_face_get_guessed_charset(face, panose);

    bool is_charset_dbcs = ass_face_is_charset_dbcs(guessed_charset, false);

    TT_OS2 *os2 = FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
    uint16_t usFirstCharIndex = os2 ? os2->usFirstCharIndex : 0;
    uint16_t fsSelection = os2 ? os2->fsSelection : 0;

    if (
        is_charset_dbcs &&
        os2 &&
        (
            os2->version == 0 ||
            ass_face_has_japanese_signature(face, os2->ulCodePageRange1)
        )
    ) {
        supported_charsets[num_charsets++] = *guessed_charset;
    } else if (os2 && os2->version > 0) {
        // If the ulCodePageRange1 contain FS_HEBREW, FS_ARABIC or FS_THAI
        // and the system charset isn't HEBREW_CHARSET, ARABIC_CHARSET or THAI_CHARSET
        // it will also add the system charset into supported_charsets.
        num_charsets = ass_face_set_charsets_from_ulCodePageRange1(os2->ulCodePageRange1, supported_charsets, false);
 
        bool supported_charsets_include_guessed_charset = false;
        for (size_t i = 0; i < num_charsets; i++) {
            if (supported_charsets[i] == *guessed_charset) {
                supported_charsets_include_guessed_charset = true;
                break;
            }
        }

        // TODO see if usefull
        if (!supported_charsets_include_guessed_charset) {
            if (num_charsets)
                *guessed_charset = supported_charsets[0];
            else
                *guessed_charset = ASS_DEFAULT_CHARSET;
        }

        // GDI also compare the ulCodePageRange2 to the result of [GetOEMCP](https://learn.microsoft.com/en-us/windows/win32/api/winnls/nf-winnls-getoemcp)
        // To compare them, if GetOEMCP return 437 and the bit 0 is set, it will add OEM_CHARSET to supported_charsets
        // Here is the corresponding list of bit versus GetOEMCP
        // Bit setted : GetOEMCP's result
        //           0: 437
        //           1: 850
        //           2: 708
        //           3: 737
        //           4: 775
        //           5: 852
        //           6: 855
        //           7: 857
        //           8: 860
        //           9: 861
        //          10: 862
        //          11: 863
        //          12: 864
        //          13: 865
        //          14: 866
        //          15: 869
    } else if (panose[ASS_bFamilyType] != ASS_PAN_FAMILY_PICTORIAL && usFirstCharIndex < 0x0100) {
        // GDI seems to do something if the cmap can be null,
        // but I don't understand in which circumstance it can be null.

        if (fsSelection & 0xFF00) {

            uint8_t charset_from_fsSelection = ((fsSelection >> 8) & 0xFF);
            switch (charset_from_fsSelection) {
                case 0xB2:
                case 0xB3:
                case 0xB4:
                    // Actually, if the EngLpkInstalled is NOT installed
                    // it will set the guessed_charset to SYMBOL_CHARSET.
                    *guessed_charset = ASS_ARABIC_CHARSET;
                    // TODO see if usefull

                    break;
                default:
                    supported_charsets[num_charsets++] = charset_from_fsSelection;
                    break;
            }
        } else {
            supported_charsets[num_charsets++] = ASS_ANSI_CHARSET;

            if (ass_font_index_magic(face, INCREMENT))
                supported_charsets[num_charsets++] = ASS_MAC_CHARSET;

            if (ass_font_index_magic(face, GREEK_CAPITAL_LETTER_OMEGA) ||
                ass_font_index_magic(face, GREEK_SMALL_LETTER_UPSILON_WITH_DIALYTIKA))
                supported_charsets[num_charsets++] = ASS_GREEK_CHARSET;

            if (ass_font_index_magic(face, LATIN_CAPITAL_LETTER_I_WITH_DOT_ABOVE))
                supported_charsets[num_charsets++] = ASS_TURKISH_CHARSET;

            if (ass_font_index_magic(face, HEBREW_LETTER_ALEF))
                supported_charsets[num_charsets++] = ASS_HEBREW_CHARSET;

            if (ass_font_index_magic(face, CYRILLIC_SMALL_LETTER_IO) ||
                ass_font_index_magic(face, CYRILLIC_CAPITAL_LETTER_YA))
                supported_charsets[num_charsets++] = ASS_RUSSIAN_CHARSET;
            
            if (ass_font_index_magic(face, LATIN_SMALL_LETTER_N_WITH_CARON) ||
                ass_font_index_magic(face, LATIN_CAPITAL_LETTER_C_WITH_CARON))
                supported_charsets[num_charsets++] = ASS_EASTEUROPE_CHARSET;

            if (ass_font_index_magic(face, LATIN_SMALL_LETTER_U_WITH_OGONEK))
                supported_charsets[num_charsets++] = ASS_BALTIC_CHARSET;

            if (ass_font_index_magic(face, MEDIUM_SHADE))
                supported_charsets[num_charsets++] = ASS_OEM_CHARSET;
        }
    } else if (usFirstCharIndex >= 0xF000 && (fsSelection & 0xFF00)) {
            uint8_t charset_from_fsselection = ((fsSelection >> 8) & 0xFF);
            switch (charset_from_fsselection) {
                case 0xB1:           
                case 0xB5:
                    supported_charsets[num_charsets++] = *guessed_charset = ASS_HEBREW_CHARSET;
                    break;
                case 0xB2:            
                case 0xB3:
                case 0xB4:
                    supported_charsets[num_charsets++] = *guessed_charset = ASS_ARABIC_CHARSET;
                    break;
            }
    } else {
        supported_charsets[num_charsets++] = *guessed_charset;
    }

    if (is_charset_dbcs && supported_charsets < 16)
        supported_charsets[num_charsets++] = ASS_FEOEM_CHARSET;
    
    // TODO see if it is a good idea
    if (!num_charsets)
        supported_charsets[num_charsets++] = *guessed_charset;

    return num_charsets;
}

/**
 * \note  Always call ass_face_has_os2_version_1_or_higher before this method.
 * \brief Get the ASS_Pitch for postscript font.
 **/
ASS_Pitch ass_face_get_pitch_postscript(FT_Face face)
{
    TT_OS2 *os2 = FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
    if (os2)
        return os2->panose[ASS_bProportion] == ASS_PAN_PROP_MONOSPACED ? ASS_FIXED_PITCH : ASS_VARIABLE_PITCH;
    
    // os2 should never be null (see note),
    // but if it happens, fallback to ASS_VARIABLE_PITCH.
    return ASS_VARIABLE_PITCH;
}

/**
 * \brief Get the ASS_Pitch for truetype font.
 **/
ASS_Pitch ass_face_get_pitch_truetype(FT_Face face, int guessed_charset,
                                      FT_Byte* panose, int *supported_charsets,
                                      size_t num_supported_charset) {
    bool is_fixed_pitch = false;

    TT_Postscript *post = FT_Get_Sfnt_Table(face, FT_SFNT_POST);
    if (post)
        is_fixed_pitch = !!post->isFixedPitch;

    if (
        (
            ass_face_is_any_charset_dbcs(supported_charsets, num_supported_charset, true) ||
            (
                guessed_charset == ASS_SHIFTJIS_CHARSET ||
                guessed_charset == ASS_HANGUL_CHARSET
            )
        ) &&
        panose[ASS_bProportion] == ASS_PAN_PROP_MONOSPACED
    )
        is_fixed_pitch = true;

    return is_fixed_pitch ? ASS_FIXED_PITCH : ASS_VARIABLE_PITCH;
}

/**
 * \note  Always call ass_face_has_os2_version_1_or_higher before this method.
 * \brief Get the ASS_Family for postscript font.
 **/
ASS_Family ass_face_get_family_postscript(FT_Face face)
{
    ASS_Family family = ASS_FF_MODERN;
    TT_OS2 *os2 = FT_Get_Sfnt_Table(face, FT_SFNT_OS2);

    if (!os2)
        // os2 should never be null (see note).
        return family;

    if (os2->panose[ASS_bFamilyType] == ASS_PAN_FAMILY_DECORATIVE ||
        os2->panose[ASS_bFamilyType] == ASS_PAN_FAMILY_PICTORIAL)
        family = ASS_FF_DECORATIVE;
    else if (os2->panose[ASS_bFamilyType] == ASS_PAN_FAMILY_SCRIPT)
        family = ASS_FF_SCRIPT;
    else if (os2->panose[ASS_bSerifStyle] >= 2 && os2->panose[ASS_bSerifStyle] <= 10)
        family = ASS_FF_ROMAN;
    else if (os2->panose[ASS_bSerifStyle] >= 11 && os2->panose[ASS_bSerifStyle] <= 15)
        family = ASS_FF_SWISS;
    return family;
}

/**
 * \brief Get the ASS_Family for truetype font.
 **/
ASS_Family ass_face_get_family_truetype(int guessed_charset, FT_Byte* panose)
{
    ASS_Family family = ASS_FF_DONTCARE;
    if (guessed_charset == ASS_SHIFTJIS_CHARSET || guessed_charset == ASS_HANGUL_CHARSET) {
        if (panose[ASS_bFamilyType] == ASS_PAN_FAMILY_SCRIPT)
            family = ASS_FF_SCRIPT;
        else if (panose[ASS_bSerifStyle] >= 2 && panose[ASS_bSerifStyle] <= 10)
            family = ASS_FF_ROMAN;
        else if (panose[ASS_bSerifStyle] >= 11 && panose[ASS_bSerifStyle] <= 15)
            family = ASS_FF_MODERN;
    } else if (panose[ASS_bFamilyType] == ASS_PAN_FAMILY_DECORATIVE)
        family = ASS_FF_DECORATIVE;
    else if (panose[ASS_bFamilyType] == ASS_PAN_FAMILY_SCRIPT)
        family = ASS_FF_SCRIPT;
    else if (panose[ASS_bProportion] == ASS_PAN_PROP_MONOSPACED)
        family = ASS_FF_MODERN;
    else if (panose[ASS_bSerifStyle] >= 2 && panose[ASS_bSerifStyle] <= 10)
        family = ASS_FF_ROMAN;
    else if (panose[ASS_bSerifStyle] >= 11 && panose[ASS_bSerifStyle] <= 15)
        family = ASS_FF_SWISS;

    return family;
}

/**
 * \brief Get maximal font ascender and descender.
 **/
void ass_font_get_asc_desc(ASS_Font *font, int face_index,
                           int *asc, int *desc)
{
    FT_Face face = font->faces[face_index];
    int y_scale = face->size->metrics.y_scale;
    *asc  = FT_MulFix(face->ascender, y_scale);
    *desc = FT_MulFix(-face->descender, y_scale);
}

/**
 * Slightly embold a glyph without touching its metrics
 */
static void ass_glyph_embolden(FT_GlyphSlot slot)
{
    int str;

    if (slot->format != FT_GLYPH_FORMAT_OUTLINE)
        return;

    str = FT_MulFix(slot->face->units_per_EM,
                    slot->face->size->metrics.y_scale) / 64;

    FT_Outline_Embolden(&slot->outline, str);
}

/**
 * Slightly italicize a glyph
 */
static void ass_glyph_italicize(FT_Face face)
{
    FT_Matrix xfrm = {
        .xx = 0x10000L,
        .yx = 0x00000L,
        .xy = ass_face_is_postscript(face)
            ? 0x02d24L /* tan(10 deg) */
            : 0x05700L /* matches GDI; effectively tan(18.77 deg) */,
        .yy = 0x10000L,
    };

    FT_Outline_Transform(&face->glyph->outline, &xfrm);
}

/**
 * \brief Get glyph and face index
 * Finds a face that has the requested codepoint and returns both face
 * and glyph index.
 */
int ass_font_get_index(ASS_FontSelector *fontsel, ASS_Font *font,
                       uint32_t symbol, int *face_index, int *glyph_index)
{
    int index = 0;
    int i;
    FT_Face face = 0;

    *glyph_index = 0;

    if (symbol < 0x20) {
        *face_index = 0;
        return 0;
    }
    if (font->n_faces == 0) {
        *face_index = 0;
        return 0;
    }

    for (i = 0; i < font->n_faces && index == 0; ++i) {
        face = font->faces[i];
        index = ass_font_index_magic(face, symbol);
        if (index)
            index = FT_Get_Char_Index(face, index);
        if (index)
            *face_index = i;
    }

    if (index == 0) {
        int face_idx;
        ass_msg(font->library, MSGL_INFO,
                "Glyph 0x%X not found, selecting one more "
                "font for (%.*s, %d, %d)", symbol, (int) font->desc.family.len, font->desc.family.str,
                font->desc.bold, font->desc.italic);
        face_idx = *face_index = add_face(fontsel, font, symbol);
        if (face_idx >= 0) {
            face = font->faces[face_idx];
            index = ass_font_index_magic(face, symbol);
            if (index)
                index = FT_Get_Char_Index(face, index);
            if (index == 0 && face->num_charmaps > 0) {
                int i;
                ass_msg(font->library, MSGL_WARN,
                    "Glyph 0x%X not found, broken font? Trying all charmaps", symbol);
                for (i = 0; i < face->num_charmaps; i++) {
                    FT_Set_Charmap(face, face->charmaps[i]);
                    index = ass_font_index_magic(face, symbol);
                    if (index)
                        index = FT_Get_Char_Index(face, index);
                    if (index) break;
                }
            }
            if (index == 0) {
                ass_msg(font->library, MSGL_ERR,
                        "Glyph 0x%X not found in font for (%.*s, %d, %d)",
                        symbol, (int) font->desc.family.len, font->desc.family.str, font->desc.bold,
                        font->desc.italic);
            }
        }
    }

    // FIXME: make sure we have a valid face_index. this is a HACK.
    *face_index  = FFMAX(*face_index, 0);
    *glyph_index = index;

    return 1;
}

/**
 * \brief Get a glyph
 * \param ch character code
 **/
bool ass_font_get_glyph(ASS_Font *font, int face_index, int index,
                        ASS_Hinting hinting)
{
    FT_Int32 flags = FT_LOAD_NO_BITMAP | FT_LOAD_IGNORE_GLOBAL_ADVANCE_WIDTH
            | FT_LOAD_IGNORE_TRANSFORM;
    switch (hinting) {
    case ASS_HINTING_NONE:
        flags |= FT_LOAD_NO_HINTING;
        break;
    case ASS_HINTING_LIGHT:
        flags |= FT_LOAD_FORCE_AUTOHINT | FT_LOAD_TARGET_LIGHT;
        break;
    case ASS_HINTING_NORMAL:
        flags |= FT_LOAD_FORCE_AUTOHINT;
        break;
    case ASS_HINTING_NATIVE:
        break;
    }

    FT_Face face = font->faces[face_index];
    FT_Error error = FT_Load_Glyph(face, index, flags);
    if (error) {
        ass_msg(font->library, MSGL_WARN, "Error loading glyph, index %d",
                index);
        return false;
    }

    FT_Long style_flags = ass_face_get_style_flags(face);
    if (!(style_flags & FT_STYLE_FLAG_ITALIC) && (font->desc.italic > 55))
        ass_glyph_italicize(face);
    if (!(style_flags & FT_STYLE_FLAG_BOLD) &&
        font->desc.bold > ass_face_get_weight(face) + 150)
        ass_glyph_embolden(face->glyph);
    return true;
}

/**
 * \brief Deallocate ASS_Font internals
 **/
void ass_font_clear(ASS_Font *font)
{
    int i;
    for (i = 0; i < font->n_faces; ++i) {
        if (font->faces[i])
            FT_Done_Face(font->faces[i]);
        if (font->hb_fonts[i])
            hb_font_destroy(font->hb_fonts[i]);
    }
    free((char *) font->desc.family.str);
}

/**
 * \brief Convert glyph into ASS_Outline according to decoration flags
 **/
bool ass_get_glyph_outline(ASS_Outline *outline, int32_t *advance,
                           FT_Face face, unsigned flags)
{
    int32_t y_scale = face->size->metrics.y_scale;
    int32_t adv = face->glyph->advance.x;
    if (flags & DECO_ROTATE)
        adv = d16_to_d6(face->glyph->linearVertAdvance);
    *advance = adv;

    int n_lines = 0;
    int32_t line_y[2][2];
    if (adv > 0 && (flags & DECO_UNDERLINE)) {
        TT_Postscript *ps = FT_Get_Sfnt_Table(face, ft_sfnt_post);
        if (ps && ps->underlinePosition <= 0 && ps->underlineThickness > 0) {
            int64_t pos  = ((int64_t) ps->underlinePosition  * y_scale + 0x8000) >> 16;
            int64_t size = ((int64_t) ps->underlineThickness * y_scale + 0x8000) >> 16;
            pos = -pos - (size >> 1);
            if (pos >= -OUTLINE_MAX && pos + size <= OUTLINE_MAX) {
                line_y[n_lines][0] = pos;
                line_y[n_lines][1] = pos + size;
                n_lines++;
            }
        }
    }
    if (adv > 0 && (flags & DECO_STRIKETHROUGH)) {
        TT_OS2 *os2 = FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
        if (os2 && os2->yStrikeoutPosition >= 0 && os2->yStrikeoutSize > 0) {
            int64_t pos  = ((int64_t) os2->yStrikeoutPosition * y_scale + 0x8000) >> 16;
            int64_t size = ((int64_t) os2->yStrikeoutSize     * y_scale + 0x8000) >> 16;
            pos = -pos - (size >> 1);
            if (pos >= -OUTLINE_MAX && pos + size <= OUTLINE_MAX) {
                line_y[n_lines][0] = pos;
                line_y[n_lines][1] = pos + size;
                n_lines++;
            }
        }
    }

    assert(face->glyph->format == FT_GLYPH_FORMAT_OUTLINE);
    FT_Outline *source = &face->glyph->outline;
    if (!source->n_points && !n_lines) {
        ass_outline_clear(outline);
        return true;
    }

    size_t max_points = 2 * source->n_points + 4 * n_lines;
    size_t max_segments = source->n_points + 4 * n_lines;
    if (!ass_outline_alloc(outline, max_points, max_segments))
        return false;

    if (!ass_outline_convert(outline, source))
        goto fail;

    if (flags & DECO_ROTATE) {
        TT_OS2 *os2 = FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
        int64_t desc = 0;
        if (os2) {
            desc = ((int64_t) os2->sTypoDescender * y_scale + 0x8000) >> 16;
            if (llabs(desc) > 2 * OUTLINE_MAX)
                goto fail;
        }
        int64_t dv = face->glyph->metrics.vertAdvance + desc;
        if (llabs(dv) > 2 * OUTLINE_MAX)
            goto fail;
        ASS_Vector offs = { dv, -desc };
        if (!ass_outline_rotate_90(outline, offs))
            goto fail;
    }

    if (!n_lines)
        return true;
    FT_Orientation dir = FT_Outline_Get_Orientation(source);
    int iy = (dir == FT_ORIENTATION_TRUETYPE ? 0 : 1);
    for (int i = 0; i < n_lines; i++)
        ass_outline_add_rect(outline, 0, line_y[i][iy], adv, line_y[i][iy ^ 1]);
    return true;

fail:
    ass_outline_free(outline);
    return false;
}
