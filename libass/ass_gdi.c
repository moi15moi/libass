/*
 * Copyright (C) 2015 Stephan Vedder <stephan.vedder@gmail.com>
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
#define COBJMACROS

#include "config.h"
#include "ass_compat.h"

#include <initguid.h>
#include <wchar.h>

#include <usp10.h>
#include <windows.h>

#include "ass_gdi.h"
#include "ass_utils.h"

#define FALLBACK_DEFAULT_FONT L"Arial"
#define TTCF_TAG 0x66637474

static const ASS_FontMapping font_substitutions[] = {
    {"sans-serif", "Arial"},
    {"serif", "Times New Roman"},
    {"monospace", "Courier New"}
};

typedef struct ASS_SharedHDC ASS_SharedHDC;

#if ASS_WINAPI_DESKTOP

struct ASS_SharedHDC {
    HDC hdc;
    unsigned ref_count;
};

static ASS_SharedHDC *hdc_retain(ASS_SharedHDC *shared_hdc)
{
    shared_hdc->ref_count++;
    return shared_hdc;
}

static void hdc_release(ASS_SharedHDC *shared_hdc)
{
    if (!--shared_hdc->ref_count) {
        DeleteDC(shared_hdc->hdc);
        free(shared_hdc);
    }
}

#endif

/*
 * The private data stored for every font, detected by this backend.
 */
typedef struct {
    ASS_SharedHDC *shared_hdc;
    bool is_postscript;
    LOGFONTW logfont;
} FontPrivate;

typedef struct {
    ASS_SharedHDC *shared_hdc;
} ProviderPrivate;


/*
 * Read a specified part of a fontfile into memory.
 * If the font wasn't used before first creates a
 * FontStream and save it into the private data for later usage.
 * If the parameter "buf" is NULL libass wants to know the
 * size of the Fontfile
 */
static size_t get_data(void *data, unsigned char *buf, size_t offset,
                       size_t length)
{
    HFONT hFont;
    DWORD result;
    FontPrivate *priv = (FontPrivate *) data;

    hFont = CreateFontIndirectW(&(priv->logfont));
    if (!hFont)
        return 0;

    HDC hdc = priv->shared_hdc->hdc;
    if (!SelectObject(hdc, hFont))
        return 0;
    
    if (buf == NULL) {
        result = GetFontData(hdc, TTCF_TAG, 0, NULL, 0);
        if (result == GDI_ERROR)
            // The file isn't a collection
            result = GetFontData(hdc, 0, 0, NULL, 0);

        if (result == GDI_ERROR)
            result = 0;
    } else {
        // There is no way to know if the font is a collection,
        // so try and see if it fails.
        result = GetFontData(hdc, TTCF_TAG, offset, buf, length);
        if (result == GDI_ERROR)
            result = GetFontData(hdc, 0, offset, buf, length);
        if (result == GDI_ERROR)
            result = 0;
    }

    DeleteObject(hFont);

    return result;
}

/*
 * Check whether the font contains PostScript outlines.
 */
static bool check_postscript(void *data)
{
    FontPrivate *priv = (FontPrivate *) data;

    return priv->is_postscript;
}

uint32_t read_uint32_from_buffer(const uint8_t* buffer) {
    return ((uint32_t)buffer[0] << 24) |
           ((uint32_t)buffer[1] << 16) |
           ((uint32_t)buffer[2] << 8)  |
           (uint32_t)buffer[3];
}

/*
 * Lazily return index of font.
 */
static unsigned get_font_index(void *data)
{
    FontPrivate *priv = (FontPrivate *)data;
    DWORD total_font_size;
    DWORD index_offset;
    HFONT hFont = NULL;
    HDC hdc = priv->shared_hdc->hdc;

    uint8_t *num_fonts_buff = NULL;
    uint8_t *table_directory_offsets_buff = NULL;
    uint32_t num_fonts = 0;
    DWORD ttc_header_size;

    unsigned font_index = 0;

    hFont = CreateFontIndirectW(&(priv->logfont));
    if (!hFont)
        goto cleanup;

    if (!SelectObject(hdc, hFont))
        goto cleanup;
    
    ttc_header_size = GetFontData(hdc, TTCF_TAG, 0, NULL, 0);
    if (ttc_header_size == GDI_ERROR)
        goto cleanup;

    // GetFontData will return the size of the selected index to the end of the ttc file
    // Ex: If the ttc file has 5 fonts and the selected font face is the index 2. The size of GetFontData will include the index 2, 3 and 4.
    // With the size, we can know what is the offset with the beginning of the file
    total_font_size = GetFontData(hdc, 0, 0, NULL, 0);
    if (total_font_size == GDI_ERROR)
        goto cleanup;

    index_offset = ttc_header_size - total_font_size;

    num_fonts_buff = malloc(ttc_header_size);
    if (!num_fonts_buff)
        goto cleanup;

    if (GetFontData(hdc, TTCF_TAG, sizeof(uint32_t) * 2 /* skip ttcTag and version */, num_fonts_buff, sizeof(num_fonts)) == GDI_ERROR)
        goto cleanup;
    
    num_fonts = read_uint32_from_buffer(num_fonts_buff);
    table_directory_offsets_buff = malloc(num_fonts * sizeof(uint32_t));
    if (!table_directory_offsets_buff)
        goto cleanup;

    if (GetFontData(hdc, TTCF_TAG, sizeof(uint32_t) * 3 /* skip ttcTag, version and numFonts */, table_directory_offsets_buff, num_fonts * sizeof(uint32_t)) == GDI_ERROR)
        goto cleanup;

    for (size_t i = 0; i < num_fonts; i++) {
        if (read_uint32_from_buffer(&table_directory_offsets_buff[i * 4]) == index_offset) {
            font_index = i;
            break;
        }
    }

cleanup:
    if (hFont)
        DeleteObject(hFont);
    if (num_fonts_buff)
        free(num_fonts_buff);
    if (table_directory_offsets_buff)
        free(table_directory_offsets_buff);
    return font_index;
}

static int encode_utf16(wchar_t *chars, uint32_t codepoint)
{
    if (codepoint < 0x10000) {
        chars[0] = codepoint;
        return 1;
    } else {
        chars[0] = (codepoint >> 10) + 0xD7C0;
        chars[1] = (codepoint & 0x3FF) + 0xDC00;
        return 2;
    }
}

/*
 * Check if the passed font has a specific unicode character.
 */
static bool check_glyph(void *data, uint32_t code)
{
    HRESULT hr = S_OK;
    FontPrivate *priv = (FontPrivate *) data;
    BOOL exists = FALSE;

    if (code == 0)
        return true;

    // Aegisub use this logic.
    // It isn't documented why ScriptGetCMap sometimes fails.
    // This should be properly tested and documented.

    wchar_t char_string[2];
    WORD indexes[2];
    int char_len = encode_utf16(char_string, code);
    hr = ScriptGetCMap(priv->shared_hdc->hdc, NULL, char_string, 2, 0, indexes);

    if (hr == S_FALSE)
        exists = false;
    else if (hr == E_HANDLE) {
        if (code >= 0x10000) {
            // GetGlyphIndicesW only support
            // so if the code has a surrogate, then GetGlyphIndicesW will
            // return garbage result
            exists = false;
        } else {
            GetGlyphIndicesW(priv->shared_hdc->hdc, char_string, 2,
                indexes, GGI_MARK_NONEXISTING_GLYPHS);
            exists = indexes[0] != 0xFFFF;
        }
    }

    return exists;
}

/*
 * This will release the gdi backend
 */
static void destroy_provider(void *priv)
{
    ProviderPrivate *provider_priv = (ProviderPrivate *)priv;
    if (provider_priv->shared_hdc != NULL)
        hdc_release(provider_priv->shared_hdc);
    free(provider_priv);
}

/*
 * This will destroy a specific font and it's
 * Fontstream (in case it does exist)
 */

static void destroy_font(void *data)
{
    FontPrivate *priv = (FontPrivate *) data;

    if (priv->shared_hdc != NULL)
        hdc_release(priv->shared_hdc);

    free(priv);
}

static char *get_utf8_name(WCHAR *string)
{
    char *mbName = NULL;

    int size_needed = WideCharToMultiByte(CP_UTF8, 0, string, -1, NULL, 0, NULL, NULL);
    if (!size_needed)
        goto cleanup;

    mbName = malloc(size_needed);
    if (!mbName)
        goto cleanup;

    WideCharToMultiByte(CP_UTF8, 0, string, -1, mbName, size_needed, NULL, NULL);

cleanup:
    return mbName;
}

static int CALLBACK font_enum_proc_is_family_exist(const ENUMLOGFONTW *lpelf,
                                   const NEWTEXTMETRICW *lpntm,
                                   DWORD FontType, LPARAM lParam)
{
    bool *is_family_exist = (bool *) lParam;
    *is_family_exist = true;
    return 0 /* stop enumerating */;
}

static int CALLBACK meta_file_enum_proc(HDC hdc, HANDLETABLE *lpht, const ENHMETARECORD *record, int nHandles, LPARAM lParam)
{
    if (record->iType == EMR_EXTCREATEFONTINDIRECTW) {
        const EMREXTCREATEFONTINDIRECTW* createFontRecord = (const EMREXTCREATEFONTINDIRECTW*) record;
        LOGFONTW *logfont = (LOGFONTW*) lParam;
        *logfont = createFontRecord->elfw.elfLogFont;
    }
    return true;
}

static char *get_fallback(void *priv, ASS_Library *lib,
                          const char *family, uint32_t codepoint)
{
    HRESULT hr;
    HFONT hFont;
    HFONT oldHFont;
    LOGFONTW lf = {0};
    HDC meta_file_dc;
    SCRIPT_STRING_ANALYSIS script_analysis = NULL;
    bool is_requested_family_exist = false;
    bool has_glyph = false;
    ProviderPrivate *provider_priv = (ProviderPrivate *)priv;

    // Encode codepoint as UTF-16
    wchar_t char_string[2];
    int char_len = encode_utf16(char_string, codepoint);

    lf.lfCharSet = DEFAULT_CHARSET;
	lf.lfOutPrecision = OUT_TT_PRECIS;
	lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	lf.lfQuality = ANTIALIASED_QUALITY;
	lf.lfPitchAndFamily = DEFAULT_PITCH|FF_DONTCARE;
    MultiByteToWideChar(CP_UTF8, 0, family, -1, lf.lfFaceName, LF_FACESIZE-1);

    EnumFontFamiliesW(provider_priv->shared_hdc->hdc, lf.lfFaceName,
                      (FONTENUMPROCW) font_enum_proc_is_family_exist, (LPARAM) &is_requested_family_exist);
    
    if (!is_requested_family_exist)
        // TODO: TO TEST. Is it really usefull to try to use the requested family or
        // we could directly try to use Arial ?
        MultiByteToWideChar(CP_UTF8, 0, FALLBACK_DEFAULT_FONT, -1, lf.lfFaceName, LF_FACESIZE-1);
    
    // From https://github.com/chromium/chromium/blob/033a2bb223be7abb7663a969ef3643cf917c8038/ui/gfx/font_fallback_win.cc#L303-L416

    meta_file_dc = CreateEnhMetaFile(provider_priv->shared_hdc->hdc, NULL, NULL, NULL);
    if (meta_file_dc == NULL)
        goto cleanup;

    hFont = CreateFontIndirect(&lf);
    if (!hFont)
        goto cleanup;

    oldHFont = SelectObject(meta_file_dc, hFont);
    if (!oldHFont || oldHFont == HGDI_ERROR)
        goto cleanup;
    
    hr = ScriptStringAnalyse(
        meta_file_dc,                                            // HDC
        char_string,                                             // pString
        lstrlenW(char_string),                                   // cString
        0,                                                       // cGlyphs
        -1,                                                      // iCharset
        SSA_METAFILE | SSA_FALLBACK | SSA_GLYPHS | SSA_LINK,     // dwFlags
        0,                                                       // iReqWidth
        NULL,                                                    // psControl
        NULL,                                                    // psState
        NULL,                                                    // piDx
        NULL,                                                    // pTabdef
        NULL,                                                    // pbInClass
        &script_analysis                                         // pssa
    );

    if (FAILED(hr))
        goto cleanup;

    HENHMETAFILE meta_file = CloseEnhMetaFile(meta_file_dc);
    if (!meta_file)
        goto cleanup;

    LOGFONTW logfont;
    EnumEnhMetaFile(0, meta_file, (ENHMFENUMPROC) meta_file_enum_proc, &logfont, NULL);

    hFont = CreateFontIndirect(&lf);
    if (!hFont)
        goto cleanup;

    oldHFont = SelectObject(provider_priv->shared_hdc->hdc, hFont);
    if (!oldHFont || oldHFont == HGDI_ERROR)
        goto cleanup;
    
    FontPrivate font_private;
    font_private.is_postscript = false; // we don't care
    font_private.logfont = logfont; 
    font_private.shared_hdc = provider_priv->shared_hdc; 
    
    has_glyph = check_glyph((void*) &font_private, codepoint);

cleanup:
    if (hFont)
        DeleteObject(hFont);

    if (script_analysis)
        ScriptStringFree(&script_analysis);
    
    if (meta_file)
        DeleteEnhMetaFile(meta_file);
    
    // Did not found an fallback
    if (!has_glyph) {
        // It seems like uniscribe return "Microsoft Sans Serif"
        // when it's unable to determine a valid fallback font.
        // If so, we may try to use font-linking.
        // But, there isn't any way to use font-linking with GDI or uniscribe.
        // We could read the registry key HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion\FontLink\SystemLink
        // like the docs says: https://learn.microsoft.com/en-us/globalization/fonts-layout/fonts
        // This is what WebKit does: https://github.com/WebKit/WebKit/blob/405d2fc1e6d8e0fac52551834bb1f57d82d27c1f/Source/WebCore/platform/graphics/win/FontCacheWin.cpp#L291-L318
        return NULL;
    }

    return get_utf8_name(logfont.lfFaceName);
}


static void add_font_face(const ENUMLOGFONTW *logfont, const NEWTEXTMETRICW *new_text_metric, ASS_FontProvider *provider,
                          ASS_SharedHDC *shared_hdc)
{
    ASS_FontProviderMetaData meta = {0};

    meta.extended_family = get_utf8_name(logfont->elfLogFont.lfFaceName);
    meta.postscript_name = get_utf8_name(logfont->elfFullName);

    FontPrivate *font_priv = calloc(1, sizeof(*font_priv));
    if (!font_priv)
        goto cleanup;

    font_priv->logfont = logfont->elfLogFont;
    font_priv->is_postscript = new_text_metric->ntmFlags & NTM_PS_OPENTYPE;
    font_priv->shared_hdc = hdc_retain(shared_hdc);

    ass_font_provider_add_font(provider, &meta, NULL, 0, font_priv);

cleanup:
    free(meta.postscript_name);
    free(meta.extended_family);
}


struct font_enum_priv {
    ASS_FontProvider *provider;
    ASS_SharedHDC *shared_hdc;
};

/*
 * Windows has three similar functions: EnumFonts, EnumFontFamilies
 * and EnumFontFamiliesEx, which were introduced at different times.
 * Each takes a callback, and the declared callback type is the same
 * for all three. However, the actual arguments passed to the callback
 * have also changed over time. Some changes match the introduction
 * of new EnumFont... variants, and some don't. Documentation has
 * changed over the years, too, so it can be hard to figure out what
 * types, and when, are safe for the callback to take.
 *
 * In the header files, FONTENUMPROC is declared to take:
 *     CONST LOGFONT *, CONST TEXTMETRIC *
 * These are the baseline structs dating back to 16-bit Windows EnumFont.
 *
 * As of 2021, current versions of Microsoft's docs
 * for the EnumFontFamiliesEx callback use the same:
 *     const LOGFONT *, const TEXTMETRIC *
 * and for the EnumFontFamilies callback use:
 *     ENUMLOGFONT *, NEWTEXTMETRIC *
 * and mention that the first "can be cast as" ENUMLOGFONTEX or ENUMLOGFONTEXDV
 * while the second "can be an ENUMTEXTMETRIC structure" but for
 * non-TrueType fonts "is a pointer to a TEXTMETRIC structure".
 *
 * Docs from the 2000s (which include Win95/98/Me/NT/2000/XP details) use
 *     ENUMLOGFONTEX *, NEWTEXTMETRICEX *
 * in the EnumFontFamiliesEx callback's definition:
 *     https://web.archive.org/web/20050907052149
 *         /http://msdn.microsoft.com/library/en-us/gdi/fontext_9rmr.asp
 * and highlight these two extended struct types as advantages over
 * the EnumFontFamilies callback, suggesting that the actual arguments
 * to the EnumFontFamiliesEx callback have always been these two extended
 * structs. This is also reflected in the callback's parameter names
 * (which have stayed unchanged in the current docs).
 * EnumFontFamiliesEx itself was added in Windows NT 4.0 and 95.
 *
 * Similarly, the EnumFontFamilies callback's parameter names support
 * the idea that they have always used ENUMLOGFONT and NEWTEXTMETRIC.
 *
 * It seems the extra fields in NEWTEXTMETRIC[EX] compared to TEXTMETRIC
 * are merely zero-filled when they are irrelevant, rather than nonexistent
 * or inaccessible. This is further supported by the fact the later-still
 * struct ENUMTEXTMETRIC extends NEWTEXTMETRICEX even though the former
 * is/was (upon introduction) relevant only to PostScript fonts
 * while the latter is (supposedly) relevant only to TrueType fonts.
 *
 * Docs from the 2000s for ENUMLOGFONTEXDV and ENUMTEXTMETRIC:
 *     https://web.archive.org/web/20050306200105
 *         /http://msdn.microsoft.com/library/en-us/gdi/fontext_15gy.asp
 * seem to assert that the callback receives these two further-extended
 * structs *if and only if* running on Windows 2000 or newer.
 * We don't need them, but if we did, we'd have to check (or assume)
 * Windows version, because this extension does not seem to have
 * an associated feature check. Moreover, these structs are given
 * to the callbacks of all three EnumFont... function variants.
 * So even EnumFonts actually uses the extended structs in 21st-century
 * Windows, but the declared callback type can no longer be changed
 * without breaking existing C++ code due to its strongly-typed pointers.
 * When targeting modern Windows, though, it seems safe for consumer
 * code to take the newest structs and cast the function pointer
 * to the declared callback type.
 */
static int CALLBACK font_enum_proc(const ENUMLOGFONTW *lpelf,
                                   const NEWTEXTMETRICW *lpntm,
                                   DWORD FontType, LPARAM lParam)
{
    struct font_enum_priv *priv = (struct font_enum_priv *) lParam;
    HFONT hFont = NULL;
    HRESULT hr;

    if (FontType & RASTER_FONTTYPE)
        goto cleanup;

    LOGFONTW lf = lpelf->elfLogFont;
    // lf.lfFaceName currently holds the font family name.
    // The family may contain several fonts with equal numerical parameters
    // but distinct full names. If we pass lf to CreateFontIndirect as is,
    // we will miss this variety and merely get the same one font multiple
    // times. Try to increase our chances of seeing all the right fonts
    // by replacing the family name by the full name. There's some chance
    // that this in turn selects some *other* family's fonts and misses
    // the requested family, but we would rather take this chance than
    // add every font twice (via the family name and via the full name).
    // lfFaceName can hold up to LF_FACESIZE wchars; truncate longer names.
    wcsncpy(lf.lfFaceName, lpelf->elfFullName, LF_FACESIZE - 1);
    lf.lfFaceName[LF_FACESIZE - 1] = L'\0';

    hFont = CreateFontIndirectW(&lf);
    if (!hFont)
        goto cleanup;

    HDC hdc = priv->shared_hdc->hdc;
    if (!SelectObject(hdc, hFont))
        goto cleanup;

    wchar_t selected_name[LF_FACESIZE];
    if (!GetTextFaceW(hdc, LF_FACESIZE, selected_name))
        goto cleanup;
    if (_wcsnicmp(selected_name, lf.lfFaceName, LF_FACESIZE)) {
        // A different font was selected. This can happen if the requested
        // name is subject to charset-specific font substitution while
        // EnumFont... enumerates additional charsets contained in the font.
        // Alternatively, the font may have disappeared in the meantime.
        // Either way, there's no use looking at this font any further.
        goto cleanup;
    }

    add_font_face(lpelf, lpntm, priv->provider, priv->shared_hdc);

cleanup:
    if (hFont)
        DeleteObject(hFont);

    return 1 /* continue enumerating */;
}

/*
 * When a new font name is requested, called to load that font from Windows
 */
static void match_fonts(void *priv, ASS_Library *lib,
                        ASS_FontProvider *provider, char *name)
{
    ProviderPrivate *provider_priv = (ProviderPrivate *)priv;
    LOGFONTW lf = {0};

    // lfFaceName can hold up to LF_FACESIZE wchars; truncate longer names
    MultiByteToWideChar(CP_UTF8, 0, name, -1, lf.lfFaceName, LF_FACESIZE-1);

    struct font_enum_priv enum_priv;

    enum_priv.shared_hdc = calloc(1, sizeof(ASS_SharedHDC));
    if (!enum_priv.shared_hdc)
        return;

    // Keep this HDC alive to keep the fonts alive. This seems to be necessary
    // on Windows 7, where the fonts can't be deleted as long as the DC lives
    // and where the converted IDWriteFontFaces only work as long as the fonts
    // aren't deleted, although not on Windows 10, where fonts can be deleted
    // even if the DC still lives but IDWriteFontFaces keep working even if
    // the fonts are deleted.
    //
    // But beware of threading: docs say CreateCompatibleDC(NULL) creates a DC
    // that is bound to the current thread and is deleted when the thread dies.
    // It's not forbidden to call libass functions from multiple threads
    // over the lifetime of a font provider, so this doesn't work for us.
    // Practical tests suggest that the docs are wrong and the DC actually
    // persists after its creating thread dies, but let's not rely on that.
    // The workaround is to do a longer dance that is effectively equivalent to
    // CreateCompatibleDC(NULL) but isn't specifically CreateCompatibleDC(NULL).
    HDC screen_dc = GetDC(NULL);
    if (!screen_dc) {
        free(enum_priv.shared_hdc);
        return;
    }
    HDC hdc = CreateCompatibleDC(screen_dc);
    ReleaseDC(NULL, screen_dc);
    if (!hdc) {
        free(enum_priv.shared_hdc);
        return;
    }

    enum_priv.provider = provider;
    enum_priv.shared_hdc->hdc = hdc;
    enum_priv.shared_hdc->ref_count = 1;

    // EnumFontFamilies gives each font once, plus repeats for charset-specific
    // aliases. EnumFontFamiliesEx gives each charset of each font separately,
    // so it repeats each font as many times as it has charsets, regardless
    // of whether they have aliases. Other than this, the two functions are
    // equivalent. There's no reliable way to distinguish when two items
    // enumerated by either function refer to the same font (but different
    // aliases or charsets) or actually distinct fonts, so we add every item
    // as a separate font to our database and simply prefer the enumeration
    // function that tends to give fewer duplicates. Generally, many fonts
    // cover multiple charsets while very few have aliases, so we prefer
    // EnumFontFamilies.
    //
    // Furthermore, the requested name might be an empty string. In this case,
    // EnumFontFamilies will give us only fonts with empty names, whereas
    // EnumFontFamiliesEx would give us a list of all installed font families.
    EnumFontFamiliesW(hdc, lf.lfFaceName,
                      (FONTENUMPROCW) font_enum_proc, (LPARAM) &enum_priv);

    hdc_release(enum_priv.shared_hdc);
}

static void get_substitutions(void *priv, const char *name,
                              ASS_FontProviderMetaData *meta)
{
    const int n = sizeof(font_substitutions) / sizeof(font_substitutions[0]);
    ass_map_font(font_substitutions, n, name, meta);
}

/*
 * Called by libass when the provider should perform the
 * specified task
 */
static ASS_FontProviderFuncs gdi_callbacks = {
    .get_data           = get_data,
    .check_postscript   = check_postscript,
    .check_glyph        = check_glyph,
    .destroy_font       = destroy_font,
    .destroy_provider   = destroy_provider,
    .match_fonts        = match_fonts,
    .get_substitutions  = get_substitutions,
    .get_fallback       = get_fallback,
    .get_font_index     = get_font_index,
};

/*
 * Register the gdi provider. Upon registering
 * scans all system fonts. The private data for this
 * provider is IDWriteFactory
 * On failure returns NULL
 */
ASS_FontProvider *ass_gdi_add_provider(ASS_Library *lib,
                                               ASS_FontSelector *selector,
                                               const char *config,
                                               FT_Library ftlib)
{
    ASS_FontProvider *provider = NULL;
    ProviderPrivate *priv = NULL;
    ASS_SharedHDC *shared_hdc = NULL;

    priv = calloc(1, sizeof(*priv));
    if (!priv)
        goto cleanup;

    shared_hdc = calloc(1, sizeof(ASS_SharedHDC));
    if (!shared_hdc)
        goto cleanup;
    
    // TODO remove duplicate code
    HDC screen_dc = GetDC(NULL);
    if (!screen_dc)
        goto cleanup;

    HDC hdc = CreateCompatibleDC(screen_dc);
    ReleaseDC(NULL, screen_dc);
    if (!hdc)
        goto cleanup;

    shared_hdc->hdc = hdc;
    shared_hdc->ref_count = 1;
    priv->shared_hdc = shared_hdc;

    provider = ass_font_provider_new(selector, &gdi_callbacks, priv);
    if (!provider)
        goto cleanup;

    return provider;

cleanup:
    free(priv);
    if (shared_hdc)
        hdc_release(shared_hdc);
    return NULL;
}
