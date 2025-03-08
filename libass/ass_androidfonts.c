/*
 * Copyright (C) 2025 libass contributors
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
#include "ass_androidfonts.h"

#ifdef CONFIG_ANDROID

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <android/api-level.h>
#include <android/asset_manager.h>
#include <android/font_matcher.h>
#include <android/font.h>
#include <android/asset_manager_jni.h>
#include <jni.h>

#include "ass_utils.h"
#include "ass.h"
#include "ass_library.h"
#include "ass_fontselect.h"
#include "ass_font.h"
#include "ass_string.h"


// TODO
static const ASS_FontMapping font_substitutions[] = {
    {"sans-serif", "Helvetica"},
    {"serif", "Times"},
    {"monospace", "Courier"}
};

static void destroy_font(void *priv)
{
    AFont *font = priv;
    AFontMatcher_destroy(font);
}

static bool check_glyph(void *priv, uint32_t code)
{
    if (code == 0)
        return true;

    // TODO
    return true;
}

static bool get_font_info(const AFont *font, char *name,
                             char **path_out,
                             ASS_FontProviderMetaData *info)
{
    char *path = AFont_getFontFilePath(font);
    *path_out = path;

    char *family_name = strdup(name);
    info->extended_family = family_name;
    if (!family_name)
        return false;

    return true;
}

static void match_fonts(void *priv, ASS_Library *lib, ASS_FontProvider *provider,
                        char *name)
{
    AFontMatcher *matcher = AFontMatcher_create();

    const uint16_t text[] = { 0x0020 };  // 0x0020 = space (' ')
    const uint32_t text_length = 1;
    uint32_t runLengthOut;

    AFont *font = AFontMatcher_match(matcher, "Roboto-Regular", text, text_length, &runLengthOut);
    size_t index = AFont_getCollectionIndex(font);


    char *path = NULL;
    ASS_FontProviderMetaData meta = {0};
    if (get_font_info(font, name, &path, &meta)) {
        ass_font_provider_add_font(provider, &meta, path, index, (void*)font);
    }
}

static char *get_fallback(void *priv, ASS_Library *lib,
                          const char *family, uint32_t codepoint)
{
    // TODO
    return NULL;
}

static void get_substitutions(void *priv, const char *name,
                              ASS_FontProviderMetaData *meta)
{
    const int n = sizeof(font_substitutions) / sizeof(font_substitutions[0]);
    ass_map_font(font_substitutions, n, name, meta);
}

static ASS_FontProviderFuncs android_callbacks = {
    .check_glyph        = check_glyph,
    .destroy_font       = destroy_font,
    .match_fonts        = match_fonts,
    .get_substitutions  = get_substitutions,
    .get_fallback       = get_fallback,
};

ASS_FontProvider *
ass_android_add_provider(ASS_Library *lib, ASS_FontSelector *selector,
                          const char *config, FT_Library ftlib)
{
    return ass_font_provider_new(selector, &android_callbacks, NULL);
}

#endif
