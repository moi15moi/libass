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

#ifndef LIBASS_FONT_H
#define LIBASS_FONT_H

#include <stdint.h>
#include <ft2build.h>
#include FT_FREETYPE_H

typedef struct ass_font ASS_Font;
typedef enum ass_panose_index ASS_PanoseIndex;
typedef enum ass_panose_family_type ASS_PanoseFamilyType;
typedef enum ass_panose_weight ASS_PanoseWeight;
typedef enum ass_panose_proportion ASS_PanoseProportion;
typedef enum ass_character_set ASS_CharacterSet;
typedef enum ass_family ASS_Family;
typedef enum ass_pitch ASS_Pitch;
typedef enum ass_cmap_type ASS_CmapType;
typedef enum ass_fs_flags ASS_FSFlags;

#define ASS_CHARACTER_SET_SIZE 20

#include "ass.h"
#include "ass_types.h"
#include "ass_fontselect.h"
#include "ass_cache.h"
#include "ass_outline.h"

#define VERTICAL_LOWER_BOUND 0x02f1

#define ASS_FONT_MAX_FACES 10
#define DECO_UNDERLINE     1
#define DECO_STRIKETHROUGH 2
#define DECO_ROTATE        4

struct ass_font {
    ASS_FontDesc desc;
    ASS_Library *library;
    FT_Library ftlibrary;
    int faces_uid[ASS_FONT_MAX_FACES];
    FT_Face faces[ASS_FONT_MAX_FACES];
    struct hb_font_t *hb_fonts[ASS_FONT_MAX_FACES];
    int n_faces;
};

enum ass_panose_index {
    ASS_bFamilyType = 0,
    ASS_bSerifStyle,
    ASS_bWeight,
    ASS_bProportion,
    ASS_bContrast,
    ASS_bStrokeVariation,
    ASS_bArmStyle,
    ASS_bLetterform,
    ASS_bMidline,
    ASS_bXHeight,
};

#define ASS_PAN_ANY    0
#define ASS_PAN_NO_FIT 1

/**
 * Same has [FamilyType](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-emf/cfb9abd0-a0a4-46df-b782-eb856e19a5d3)
 */
enum ass_panose_family_type {
    ASS_PAN_FAMILY_TEXT_DISPLAY = 2,
    ASS_PAN_FAMILY_SCRIPT,
    ASS_PAN_FAMILY_DECORATIVE,
    ASS_PAN_FAMILY_PICTORIAL
};

/**
 * Same has [Weight](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-emf/46caba8a-83e5-450d-8c4d-d3999be38b93)
 */
enum ass_panose_weight {
    ASS_PAN_WEIGHT_VERY_LIGHT = 2,
    ASS_PAN_WEIGHT_LIGHT,
    ASS_PAN_WEIGHT_THIN,
    ASS_PAN_WEIGHT_BOOK,
    ASS_PAN_WEIGHT_MEDIUM,
    ASS_PAN_WEIGHT_DEMI,
    ASS_PAN_WEIGHT_BOLD,
    ASS_PAN_WEIGHT_HEAVY,
    ASS_PAN_WEIGHT_BLACK,
    ASS_PAN_WEIGHT_NORD
};

/**
 * Same has [Proportion](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-emf/d59152c4-deb0-49d8-8eeb-0407167698fe)
 */
enum ass_panose_proportion {
    ASS_PAN_PROP_OLD_STYLE = 2,
    ASS_PAN_PROP_MODERN,
    ASS_PAN_PROP_EVEN_WIDTH,
    ASS_PAN_PROP_EXPANDED,
    ASS_PAN_PROP_CONDENSED,
    ASS_PAN_PROP_VERY_EXPANDED,
    ASS_PAN_PROP_VERY_CONDENSED,
    ASS_PAN_PROP_MONOSPACED
};

/**
 * Similar has [CharacterSet](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-wmf/0d0b32ac-a836-4bd2-a112-b6000a1b4fc9)
 */
enum ass_character_set {
    ASS_ANSI_CHARSET        = 0,
    ASS_DEFAULT_CHARSET     = 1,
    ASS_SYMBOL_CHARSET      = 2,
    ASS_MAC_CHARSET         = 77,
    ASS_SHIFTJIS_CHARSET    = 128,
    ASS_HANGUL_CHARSET      = 129,
    ASS_JOHAB_CHARSET       = 130,
    ASS_GB2312_CHARSET      = 134,
    ASS_CHINESEBIG5_CHARSET = 136,
    ASS_GREEK_CHARSET       = 161,
    ASS_TURKISH_CHARSET     = 162,
    ASS_VIETNAMESE_CHARSET  = 163,
    ASS_HEBREW_CHARSET      = 177,
    ASS_ARABIC_CHARSET      = 178,
    ASS_BALTIC_CHARSET      = 186,
    ASS_RUSSIAN_CHARSET     = 204,
    ASS_THAI_CHARSET        = 222,
    ASS_EASTEUROPE_CHARSET  = 238,
    ASS_FEOEM_CHARSET       = 254, // Not documented by Microsoft
    ASS_OEM_CHARSET         = 255
};

/**
 * Same has [FamilyFont](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-wmf/9a632766-1f1c-4e2b-b1a4-f5b1a45f99ad)
 */
enum ass_family {
    ASS_FF_DONTCARE   = 0 << 4,
    ASS_FF_ROMAN      = 1 << 4,
    ASS_FF_SWISS      = 2 << 4,
    ASS_FF_MODERN     = 3 << 4,
    ASS_FF_SCRIPT     = 4 << 4,
    ASS_FF_DECORATIVE = 5 << 4,
};

/**
 * Same has [PitchFont](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-wmf/22dbe377-aec4-4669-88e6-b8fdd9351d76)
 */
enum ass_pitch {
    ASS_DEFAULT_PITCH = 0,
    ASS_FIXED_PITCH,
    ASS_VARIABLE_PITCH,
};

enum ass_cmap_type {
    ASS_CMAP_UNKNOWN = 0,
    ASS_CMAP_UNICODE,
    ASS_CMAP_NOT_UNICODE,
    ASS_CMAP_HIGH_BYTE,
    ASS_CMAP_SYMBOL,
    ASS_CMAP_MAC_ROMAN,
    ASS_CMAP_WIN_ANSI,
};

/**
 * Flags for ulCodePageRange1 attribute of the OS/2 table.
 */
enum ass_fs_flags {
    ASS_FS_LATIN1      = 1 << 0,
    ASS_FS_LATIN2      = 1 << 1,
    ASS_FS_CYRILLIC    = 1 << 2,
    ASS_FS_GREEK       = 1 << 3,
    ASS_FS_TURKISH     = 1 << 4,
    ASS_FS_HEBREW      = 1 << 5,
    ASS_FS_ARABIC      = 1 << 6,
    ASS_FS_BALTIC      = 1 << 7,
    ASS_FS_VIETNAMESE  = 1 << 8,
    ASS_FS_THAI        = 1 << 16,
    ASS_FS_JISJAPAN    = 1 << 17,
    ASS_FS_CHINESESIMP = 1 << 18,
    ASS_FS_WANSUNG     = 1 << 19,
    ASS_FS_CHINESETRAD = 1 << 20,
    ASS_FS_JOHAB       = 1 << 21,
    ASS_FS_FEOEM       = 1 << 26,
    ASS_FS_MAC         = 1 << 29,
    ASS_FS_SYMBOL      = 1 << 31,
};

#define HALFWIDTH_KATAKANA_LETTER_A               0xFF71
#define HALFWIDTH_KATAKANA_LETTER_I               0xFF72
#define HALFWIDTH_KATAKANA_LETTER_U               0xFF73
#define HALFWIDTH_KATAKANA_LETTER_E               0xFF74
#define HALFWIDTH_KATAKANA_LETTER_O               0xFF75
         
#define CJK_UNIFIED_IDEOGRAPH_61D4                0x61D4
#define CJK_UNIFIED_IDEOGRAPH_9EE2                0x9EE2 
         
#define CJK_UNIFIED_IDEOGRAPH_9F79                0x9F79
#define CJK_UNIFIED_IDEOGRAPH_9F98                0x9F98  
         
#define HANGUL_SYLLABLE_GA                        0xAC00   
#define HANGUL_SYLLABLE_HA                        0xD558   
         
#define PRIVATE_USE_AREA_E000                     0xE000
         
#define INCREMENT                                 0x2206

#define GREEK_CAPITAL_LETTER_OMEGA                0x03A9
#define GREEK_SMALL_LETTER_UPSILON_WITH_DIALYTIKA 0x03CB

#define LATIN_CAPITAL_LETTER_I_WITH_DOT_ABOVE     0x0130

#define HEBREW_LETTER_ALEF                        0x05D0

#define CYRILLIC_SMALL_LETTER_IO                  0x0451
#define CYRILLIC_CAPITAL_LETTER_YA                0x042F

#define LATIN_SMALL_LETTER_N_WITH_CARON           0x0148
#define LATIN_CAPITAL_LETTER_C_WITH_CARON         0x010C

#define LATIN_SMALL_LETTER_U_WITH_OGONEK          0x0173

#define MEDIUM_SHADE                              0x2592


void ass_charmap_magic(ASS_Library *library, FT_Face face);
ASS_Font *ass_font_new(ASS_Renderer *render_priv, ASS_FontDesc *desc);
void ass_face_set_size(FT_Face face, double size);
int ass_face_get_weight(FT_Face face);
FT_Long ass_face_get_style_flags(FT_Face face);
bool ass_face_is_postscript(FT_Face face);
ASS_Pitch ass_face_get_pitch_postscript(FT_Face face);
ASS_Family ass_face_get_family_postscript(FT_Face face);
size_t ass_face_get_charsets_postscript(FT_Face face, ASS_CharacterSet *supported_charsets);
ASS_Pitch ass_face_get_pitch_truetype(FT_Face face, ASS_CharacterSet guessed_charset, FT_Byte* panose, ASS_CharacterSet *supported_charsets, size_t num_supported_charset);
FT_Byte* ass_face_set_panose(FT_Face face, FT_Long style_flags, FT_Bytes *panose);
bool ass_face_has_os2_version_1_or_higher(FT_Face face);
ASS_CharacterSet ass_face_get_guessed_charset(FT_Face face, FT_Byte* panose);
ASS_Family ass_face_get_family_truetype(ASS_CharacterSet guessed_charset, FT_Byte* panose);
size_t ass_face_get_charsets_truetype(FT_Face face, ASS_CharacterSet guessed_charset, FT_Byte* panose, ASS_CharacterSet *supported_charsets);
void ass_font_get_asc_desc(ASS_Font *font, int face_index,
                           int *asc, int *desc);
int ass_font_get_index(ASS_FontSelector *fontsel, ASS_Font *font,
                       uint32_t symbol, int *face_index, int *glyph_index);
uint32_t ass_font_index_magic(FT_Face face, uint32_t symbol);
bool ass_font_get_glyph(ASS_Font *font, int face_index, int index,
                        ASS_Hinting hinting);
void ass_font_clear(ASS_Font *font);

bool ass_get_glyph_outline(ASS_Outline *outline, int32_t *advance,
                           FT_Face face, unsigned flags);

FT_Face ass_face_open(ASS_Library *lib, FT_Library ftlib, const char *path,
                      const char *postscript_name, int index);
FT_Face ass_face_stream(ASS_Library *lib, FT_Library ftlib, const char *name,
                        const ASS_FontStream *stream, int index);

#endif                          /* LIBASS_FONT_H */
