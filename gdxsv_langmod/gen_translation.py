# gdxsv_translation.py
# Created by Edward Li on 3/6/2021.
# Copyright 2022 gdxsv. All rights reserved.

import codecs
import csv
import os
import sys
import time

os.chdir(sys.path[0])
lang_patch_id = str(int(time.time()) % 100000000)
f = codecs.open("../core/gdxsv/gdxsv_translation_patch.inc", "w", encoding="shift_jis_2004")
f.write("""\
// *WARNING* DO NOT EDIT BY HAND
// This file is auto-generated by a tool gdxsv_translation.py.

#define TRANSLATE(offset,length,original,cantonese,english) GdxsvTranslationWithMaxLength<length>(offset,original,cantonese,english)
#define CUSTOMIZE(offset,length,original,cantonese,english,japanese) GdxsvTranslationWithMaxLength<length>(offset,original,cantonese,english,japanese)

const static GdxsvTranslation translations_disk2[] = {
""")

with open('translation.csv', encoding="utf8") as csvfile:
    for row in csv.DictReader(csvfile):
        if row['Address'].startswith('//'):
            continue
        args = (row["Address"], row["MaxLength"], row["Original"],
                row["Cantonese"], row["English"], row["Patched Japanese"])
        if row['Patched Japanese']:
            f.write('    CUSTOMIZE({0}, {1}, R"({2})", R"({3})", R"({4})", R"({5})"),\n'.format(*args))
        else:
            f.write('    TRANSLATE({0}, {1}, R"({2})", R"({3})", R"({4})"),\n'.format(*args[:-1]))
f.write("""};

#undef TRANSLATE
#undef CUSTOMIZE

if (disk == 2) {
    for (const auto& translation : translations_disk2) {
        const static u32 offset = 0x8C000000 + 0x00010000;
        const char * text = translation.Text();
        if (!text) continue;
        const auto length = strlen(text);
        for (int i = 0; i < length; ++i) {
            gdxsv_WriteMem8(offset + translation.offset + i, u8(text[i]));
        }
        gdxsv_WriteMem8(offset + translation.offset + (u32)length, u8(0));
    }
    
    // To manage lang-path version, overwrite "UNUSED" text area
    symbols["lang_patch_id"] = 0x0c1d37dc;
    symbols[":lang_patch_id"] = """ + lang_patch_id + """;
    symbols[":lang_patch_lang"] = (u32)GdxsvLanguage::Language();
    gdxsv_WriteMem32(symbols["lang_patch_id"], symbols[":lang_patch_id"]);
}
""")
f.close()
