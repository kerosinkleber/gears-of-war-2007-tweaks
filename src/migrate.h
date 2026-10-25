// Earlier versions of the mod were called gowfov. On the first start of this version (from the
// mod or the launcher, whichever runs first) its files are moved to the new names: the settings
// file (with its [gowfov] section renamed to [rowe]; the old file is kept as gowfov.ini.old), the
// save slots folder and the launcher background. Nothing happens if the new name already exists.
#pragma once
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// dir: the Binaries folder with trailing backslash. Returns true if anything was moved.
static bool MigrateGowfovFiles(const char* dir)
{
    char from[MAX_PATH], to[MAX_PATH];
    bool moved = false;
    wsprintfA(from, "%sgowfov.ini", dir);
    wsprintfA(to, "%srowe.ini", dir);
    if (GetFileAttributesA(to) == INVALID_FILE_ATTRIBUTES && GetFileAttributesA(from) != INVALID_FILE_ATTRIBUTES) {
        FILE* in = fopen(from, "rb");
        if (in) {
            fseek(in, 0, SEEK_END);
            long n = ftell(in);
            fseek(in, 0, SEEK_SET);
            char* text = (char*)malloc(n + 1);
            if (text) {
                size_t got = fread(text, 1, n, in);
                text[got] = 0;
                FILE* out = fopen(to, "wb");
                if (out) {
                    const char* p = text;
                    const char* hit;
                    while ((hit = strstr(p, "[gowfov]")) != nullptr) {  // the section header
                        fwrite(p, 1, hit - p, out);
                        fputs("[rowe]", out);
                        p = hit + 8;
                    }
                    fputs(p, out);
                    fclose(out);
                    moved = true;
                }
                free(text);
            }
            fclose(in);
            if (moved) {
                char old[MAX_PATH];
                wsprintfA(old, "%sgowfov.ini.old", dir);
                MoveFileExA(from, old, MOVEFILE_REPLACE_EXISTING);
            }
        }
    }
    static const char* kRenames[][2] = {{"gowfov_saves", "rowe_saves"}, {"gowfov_menubg.jpg", "rowe_menubg.jpg"}};
    for (auto& r : kRenames) {
        wsprintfA(from, "%s%s", dir, r[0]);
        wsprintfA(to, "%s%s", dir, r[1]);
        if (GetFileAttributesA(to) == INVALID_FILE_ATTRIBUTES && GetFileAttributesA(from) != INVALID_FILE_ATTRIBUTES)
            moved |= MoveFileA(from, to) != FALSE;
    }
    return moved;
}
