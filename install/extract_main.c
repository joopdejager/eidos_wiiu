/*
 * install/extract_main.c -- eidos_extract, the extractor on the command line.
 *
 *     eidos_extract <rom.nds>                      identify only
 *     eidos_extract <rom.nds> <out_dir>            identify, then extract
 *     eidos_extract --replace <rom.nds> <out_dir>  replace an extraction
 *                                                  of a different ROM
 *     eidos_extract --any <rom.nds> <out_dir>      extract an image that is
 *                                                  not the supported dump
 *
 * The installer's front ends (the wizard on each platform) call the same
 * three functions this does (identify, verdict, extract) and show the same
 * messages; this is the version with no user interface beyond a line of
 * progress, for scripting and for checking a dump.  Exit status: 0
 * extracted, already extracted, or identified the supported dump; 1
 * refused or failed; 2 usage.
 */

#include <stdio.h>
#include <string.h>

#include "install.h"

#ifdef _WIN32
#include <stdlib.h>
#include <windows.h>
#include <shellapi.h>

/*
 * On Windows `argv` is in the ANSI code page (1252 on a Western install),
 * not UTF-8, so a ROM in "C:\\Users\\Andre\\Téléchargements" would arrive
 * as bytes install/ cannot open.  Take the command line as UTF-16 and hand
 * the library UTF-8, which is what every bw_ path function expects.
 */
static char **utf8_argv(int *argc)
{
    LPWSTR *w = CommandLineToArgvW(GetCommandLineW(), argc);
    char **out;
    int i;

    if (w == NULL) {
        return NULL;
    }
    out = (char **)calloc((size_t)*argc + 1, sizeof(char *));
    for (i = 0; out != NULL && i < *argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, NULL, 0, NULL, NULL);
        out[i] = (char *)malloc((size_t)n);
        if (out[i] == NULL) {
            return NULL;
        }
        WideCharToMultiByte(CP_UTF8, 0, w[i], -1, out[i], n, NULL, NULL);
    }
    LocalFree(w);
    return out;
}
#endif

static int progress(void *ctx, u32 done, u32 total, const char *what)
{
    static int last = -1;
    static const char *phase = NULL;
    const char *label = strcmp(what, "hashing") == 0 ? "hashing   " : (const char *)ctx;
    int pct = total ? (int)((u64)done * 100u / total) : 100;

    if (label != phase) {
        phase = label;
        last = -1;
    }
    if (pct != last) {
        fprintf(stderr, "\r  %s %3d%%", label, pct);
        if (pct == 100) {
            fputc('\n', stderr);
        }
        fflush(stderr);
        last = pct;
    }
    return 0;
}

int main(int argc, char **argv)
{
    BwRomInfo info;
    BwExtractOptions opt;
    BwExtractResult res;
    BwRomVerdict verdict;
    char err[1024];
    char msg[512];
    int any = 0;
    int replace = 0;
    int a = 1;

#ifdef _WIN32
    {
        char **u = utf8_argv(&argc);
        if (u != NULL) {
            argv = u;
        }
    }
#endif

    if (argc == 3 && strcmp(argv[1], "--to-host-order") == 0) {
        /* A tree extracted elsewhere (aletheia's ndsrom.py, or another
         * host): its NARCs' framing into this host's order. */
        char err[512];
        u32 n = 0;

        if (bw_tree_to_host_order(argv[2], &n, err, sizeof(err)) != 0) {
            fprintf(stderr, "eidos_extract: %s\n", err);
            return 1;
        }
        printf("eidos_extract: %lu file(s) in %s now in this host's byte order%s\n",
               (unsigned long)n, argv[2], BW_HOST_BIG_ENDIAN ? "" : " (little-endian: none needed)");
        return 0;
    }
    for (; a < argc && strncmp(argv[a], "--", 2) == 0; a++) {
        if (strcmp(argv[a], "--any") == 0) {
            any = 1;
        } else if (strcmp(argv[a], "--replace") == 0) {
            replace = 1;
        } else {
            argc = 0;                   /* unknown option: usage */
            break;
        }
    }
    if (argc - a < 1 || argc - a > 2) {
        fprintf(stderr, "usage: eidos_extract [--any] [--replace] <rom.nds> [out_dir]\n"
                        "       eidos_extract --to-host-order <extract_dir>\n");
        return 2;
    }

    if (bw_rom_identify(argv[a], &info, progress, (void *)"hashing   ",
                        err, sizeof(err)) != 0) {
        fprintf(stderr, "eidos_extract: %s\n", err);
        return 1;
    }
    verdict = bw_rom_verdict(&info, msg, sizeof(msg));
    printf("title     %s\n", info.title);
    printf("gamecode  %s (maker %s, unit code 0x%02X%s)\n", info.gamecode,
           info.maker, info.unit_code, (info.unit_code & 2) ? ", DSi enhanced" : "");
    printf("size      %u bytes%s\n", (unsigned)info.rom_size,
           info.trimmed ? " (trimmed)" : "");
    printf("sha1      %s\n", info.sha1_hex);
    if (info.trimmed) {
        printf("padded    %s\n", info.padded_sha1_hex);
    }
    printf("verdict   %s\n", msg);
    fflush(stdout);

    if (argc - a == 1) {
        return verdict == BW_VERDICT_OK ? 0 : 1;
    }
    if (verdict != BW_VERDICT_OK) {
        if (!any) {
            fprintf(stderr, "eidos_extract: refused (pass --any to extract it "
                            "anyway, for testing only)\n");
            return 1;
        }
        fprintf(stderr, "eidos_extract: warning: extracting an image that is "
                        "not the supported dump (--any)\n");
    }

    memset(&opt, 0, sizeof(opt));
    opt.require_gamecode = any ? NULL : "IRBO";
    /* A dump trimmed of its padding records the hash of the whole dump,
     * as the wizard does (bw_setup_identity), so either front end's
     * extraction is recognised as the known dump's. */
    opt.rom_sha1_hex = (info.trimmed && info.verified) ? info.padded_sha1_hex
                                                       : info.sha1_hex;
    opt.replace = replace;
    if (bw_rom_extract(argv[a], argv[a + 1], &opt, &res, progress,
                       (void *)"extracting", err, sizeof(err)) != 0) {
        fprintf(stderr, "\neidos_extract: %s\n", err);
        if (strstr(err, "different ROM") != NULL) {
            fprintf(stderr, "eidos_extract: pass --replace to replace it\n");
        }
        return 1;
    }
    if (res.already) {
        printf("already extracted: %s holds this ROM's extraction; nothing written\n",
               argv[a + 1]);
        return 0;
    }
    printf("extracted %u files (%u NitroFS, %u overlays, %u decompressed "
           "overlays, %u orphans), %llu bytes; arm9_dec.bin %u bytes\n",
           (unsigned)res.regions, (unsigned)res.fs_files, (unsigned)res.overlays,
           (unsigned)res.overlays_dec, (unsigned)res.orphans,
           (unsigned long long)res.bytes, (unsigned)res.arm9_dec_size);
    return 0;
}
