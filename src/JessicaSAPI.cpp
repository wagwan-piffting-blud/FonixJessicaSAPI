// JessicaSAPI.cpp - Fonix Jessica SAPI5 TTS engine
//
// ISpTTSEngine over FAAST v520 LvCore.dll synthText(), speaking with the Jessica
// Pocket PC sound bank. Layout relative to this DLL:
//     lib                      LvCore.dll + Fonix runtime DLLs
//     data\Jessica\cb112298    sound bank
//     data\dictfls             dictionaries
//     data\userdict            user dictionary
//
// Build: build.bat at the repo root (MSVC x86, /MT).

#define WIN32_LEAN_AND_MEAN
#define INITGUID
#define _USE_MATH_DEFINES
#include <windows.h>
#include <objbase.h>
#include <sapi.h>
#include <sapiddk.h>
#include <sperror.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// {B3478C8E-CA57-4152-B62E-2CAC971CEC7F}
static const CLSID CLSID_FonixJessica = {
    0xB3478C8E, 0xCA57, 0x4152,
    {0xB6, 0x2E, 0x2C, 0xAC, 0x97, 0x1C, 0xEC, 0x7F}
};
static const char CLSID_STR[]  = "{B3478C8E-CA57-4152-B62E-2CAC971CEC7F}";
static const char TOKEN_KEY[]  = "SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens\\FonixJessica";
static const char VOICE_NAME[] = "Fonix Jessica";

static const WORD  OUTPUT_CHANNELS = 1;
static const DWORD OUTPUT_RATE     = 16000;
static const WORD  OUTPUT_BITS     = 16;

// ---------- DLL globals ----------
static HMODULE          g_hThisDll   = NULL;
static LONG             g_cRefDll    = 0;
static CRITICAL_SECTION g_csEngine;
static HMODULE          g_hLvCore    = NULL;
static void*            g_pSoundbank = NULL;
static char             g_thisDllPath[MAX_PATH] = {0};

static char LIB_DIR[MAX_PATH];
static char LVCORE_PATH[MAX_PATH];
static char SNDBANK_DIR[MAX_PATH];
static char DICTFLS_DIR[MAX_PATH];
static char USERDICT_DIR[MAX_PATH];

typedef int (__cdecl *openSoundbankEx_t)(void** sb, const char* sndbank, const char* dictfls, const char* userdict);
typedef int (__cdecl *synthText_t)(void* sb, char** out, unsigned int* outsize, const char* input, int flags);
typedef int (__cdecl *freeSoundBuffer_t)(void* mem);
static openSoundbankEx_t fOpenSoundbank   = NULL;
static synthText_t       fSynthText       = NULL;
static freeSoundBuffer_t fFreeSoundBuffer = NULL;

static void ComputePaths()
{
    char installDir[MAX_PATH];
    strncpy_s(installDir, MAX_PATH, g_thisDllPath, _TRUNCATE);
    char* lastSlash = strrchr(installDir, '\\');
    if (lastSlash) {
        *lastSlash = '\0';
    }
    snprintf(LIB_DIR,      MAX_PATH, "%s\\lib",                       installDir);
    snprintf(LVCORE_PATH,  MAX_PATH, "%s\\lib\\LvCore.dll",           installDir);
    snprintf(SNDBANK_DIR,  MAX_PATH, "%s\\data\\Jessica\\cb112298\\", installDir);
    snprintf(DICTFLS_DIR,  MAX_PATH, "%s\\data\\dictfls\\",           installDir);
    snprintf(USERDICT_DIR, MAX_PATH, "%s\\data\\userdict\\",          installDir);
}

// ---------- Cybit64 decoder hook ----------
// LvCore v520 carries the Pocket PC codec but drops the device's per-unit fades. src\cybit.c
// restores them and adds a stronger tilt plus makeup gain. Voice token DWORDs CybitTilt (Q12,
// signed), CybitGain (Q12) and CybitFade (0/1) override the defaults.
#include "cybit.h"

static CybitOptions g_cybit;

static void __cdecl CybitHook(void* /*lvState*/, char* out, int* outBytes, short* in, unsigned inBytes)
{
    *outBytes = cybit_decode_unit((const uint8_t*)in, inBytes, (int16_t*)out, &g_cybit) * 2;
}

static bool ReadTokenDword(HKEY h, const char* name, DWORD* value)
{
    DWORD type, cb = sizeof(DWORD);
    return RegQueryValueExA(h, name, NULL, &type, (BYTE*)value, &cb) == ERROR_SUCCESS && type == REG_DWORD;
}

static void LoadCybitOptions()
{
    g_cybit = CYBIT_PPC;
    g_cybit.tilt = -2867;
    g_cybit.out_gain = 0x2000;
    HKEY h;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, TOKEN_KEY, 0, KEY_READ, &h) != ERROR_SUCCESS) {
        return;
    }
    DWORD v;
    if (ReadTokenDword(h, "CybitTilt", &v)) {
        g_cybit.tilt = (int16_t)(int32_t)v;
    }
    if (ReadTokenDword(h, "CybitGain", &v)) {
        g_cybit.out_gain = (int16_t)v;
    }
    if (ReadTokenDword(h, "CybitFade", &v)) {
        g_cybit.unit_fade = v != 0;
    }
    RegCloseKey(h);
}

// Redirects LvCore's CALL to its Cybit64 unit decoder; an unrecognised LvCore build keeps the stock decoder.
static void InstallCybitHook(HMODULE lv)
{
    BYTE* site = (BYTE*)lv + CYBIT_LVCORE_CALL_RVA;
    int32_t rel;
    memcpy(&rel, site + 1, 4);
    if (site[0] != 0xE8 || site + 5 + rel != (BYTE*)lv + CYBIT_LVCORE_DECODER_RVA) {
        return;
    }
    LoadCybitOptions();
    rel = (int32_t)((BYTE*)CybitHook - (site + 5));
    DWORD old;
    if (VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) {
        memcpy(site + 1, &rel, 4);
        VirtualProtect(site, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), site, 5);
    }
}

// ---------- LvCore lifetime (caller holds g_csEngine) ----------
static HRESULT EnsureLvCoreLoaded()
{
    if (g_pSoundbank) {
        return S_OK;
    }
    if (!g_hLvCore) {
        // LvCore's only non-system imports (FTKcUtils.dll, MemLib.dll) resolve from lib; it never LoadLibrary()s later.
        char prevDir[MAX_PATH];
        DWORD hadPrev = GetDllDirectoryA(MAX_PATH, prevDir);
        SetDllDirectoryA(LIB_DIR);
        HMODULE h = LoadLibraryA(LVCORE_PATH);
        DWORD err = GetLastError();
        SetDllDirectoryA(hadPrev ? prevDir : NULL);
        if (!h) {
            return HRESULT_FROM_WIN32(err);
        }
        fOpenSoundbank   = (openSoundbankEx_t)GetProcAddress(h, "openSoundbankEx");
        fSynthText       = (synthText_t)GetProcAddress(h, "synthText");
        fFreeSoundBuffer = (freeSoundBuffer_t)GetProcAddress(h, "freeSoundBuffer");
        if (!fOpenSoundbank || !fSynthText || !fFreeSoundBuffer) {
            FreeLibrary(h);
            return E_FAIL;
        }
        InstallCybitHook(h);
        g_hLvCore = h;
    }
    void* sb = NULL;
    if (fOpenSoundbank(&sb, SNDBANK_DIR, DICTFLS_DIR, USERDICT_DIR) != 0 || !sb) {
        return E_FAIL;
    }
    g_pSoundbank = sb;
    return S_OK;
}

static int GuardedSynthText(const char* text, int flags, char** wav, unsigned int* size, DWORD* exc)
{
    *exc = 0;
    __try {
        return fSynthText(g_pSoundbank, wav, size, text, flags);
    } __except (*exc = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// ---------- Audio helpers ----------
static bool FindPcm(const char* wav, unsigned int size, const short** pcm, ULONG* samples, DWORD* rate)
{
    if (size < 12 || memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0) {
        return false;
    }
    bool fmtOk = false;
    unsigned int pos = 12;
    while (pos + 8 <= size) {
        unsigned int ckSize;
        memcpy(&ckSize, wav + pos + 4, 4);
        if (memcmp(wav + pos, "fmt ", 4) == 0 && ckSize >= 16 && pos + 24 <= size) {
            WORD channels, bits;
            memcpy(&channels, wav + pos + 10, 2);
            memcpy(rate, wav + pos + 12, 4);
            memcpy(&bits, wav + pos + 22, 2);
            fmtOk = (channels == 1 && bits == 16);
        } else if (memcmp(wav + pos, "data", 4) == 0) {
            if (!fmtOk) {
                return false;
            }
            unsigned int avail = size - (pos + 8);
            if (ckSize > avail) {
                ckSize = avail;
            }
            *pcm = (const short*)(wav + pos + 8);
            *samples = ckSize / 2;
            return true;
        }
        if (ckSize > size - pos - 8) {
            break;
        }
        pos += 8 + ckSize + (ckSize & 1);
    }
    return false;
}

// 8 -> 16 kHz: even outputs pass the input through, odd outputs come from a
// Blackman-windowed half-band sinc spanning HB_TAPS input samples per side.
static const int HB_TAPS = 12;
static double g_halfband[HB_TAPS];

static void InitHalfband()
{
    double edge = HB_TAPS + 0.5;
    double sum = 0.0;
    for (int k = 0; k < HB_TAPS; ++k) {
        double t = k + 0.5;
        double sinc = sin(M_PI * t) / (M_PI * t);
        double win = 0.42 + 0.5 * cos(M_PI * t / edge) + 0.08 * cos(2.0 * M_PI * t / edge);
        g_halfband[k] = sinc * win;
        sum += 2.0 * g_halfband[k];
    }
    for (int k = 0; k < HB_TAPS; ++k) {
        g_halfband[k] /= sum;
    }
}

static short* Upsample2x(const short* src, ULONG n)
{
    short* dst = (short*)malloc((size_t)n * 2 * sizeof(short));
    if (!dst) {
        return NULL;
    }
    for (ULONG i = 0; i < n; ++i) {
        double acc = 0.0;
        for (int k = 0; k < HB_TAPS; ++k) {
            long a = (long)i - k;
            ULONG b = i + 1 + k;
            acc += g_halfband[k] * ((a >= 0 ? src[a] : 0) + (b < n ? src[b] : 0));
        }
        long v = lround(acc);
        dst[2 * i]     = src[i];
        dst[2 * i + 1] = (short)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
    }
    return dst;
}

// Synthesizes one ANSI string to malloc'd 16 kHz PCM. S_FALSE = nothing produced.
static HRESULT SynthToPcm(const char* text, int flags, short** pcmOut, ULONG* samplesOut)
{
    *pcmOut = NULL;
    *samplesOut = 0;

    EnterCriticalSection(&g_csEngine);
    HRESULT hr = EnsureLvCoreLoaded();
    if (FAILED(hr)) {
        LeaveCriticalSection(&g_csEngine);
        return hr;
    }

    char* wav = NULL;
    unsigned int size = 0;
    DWORD exc;
    int rc = GuardedSynthText(text, flags, &wav, &size, &exc);
    hr = S_FALSE;
    if (exc != 0) {
        // Engine state is suspect after a fault: abandon this soundbank and reopen on the next call.
        g_pSoundbank = NULL;
    } else {
        const short* src = NULL;
        ULONG n = 0;
        DWORD rate = 0;
        if (rc == 0 && wav && FindPcm(wav, size, &src, &n, &rate) && n > 0) {
            if (rate == 8000) {
                *pcmOut = Upsample2x(src, n);
                *samplesOut = n * 2;
            } else if (rate == OUTPUT_RATE) {
                *pcmOut = (short*)malloc((size_t)n * sizeof(short));
                if (*pcmOut) {
                    memcpy(*pcmOut, src, (size_t)n * sizeof(short));
                }
                *samplesOut = n;
            }
            if (*pcmOut) {
                hr = S_OK;
            }
        }
        if (wav) {
            fFreeSoundBuffer(wav);
        }
    }
    LeaveCriticalSection(&g_csEngine);
    return hr;
}

// UTF-16 -> ANSI for LvCore. Typographic punctuation and control characters are
// flattened to ASCII first; SpellOut separates every character.
static char* ToEngineText(const WCHAR* src, ULONG len, bool spellOut)
{
    WCHAR* w = (WCHAR*)malloc(((size_t)len * 4 + 1) * sizeof(WCHAR));
    if (!w) {
        return NULL;
    }
    ULONG n = 0;
    for (ULONG i = 0; i < len; ++i) {
        WCHAR c = src[i];
        switch (c) {
        case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032:
            c = L'\'';
            break;
        case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033:
            c = L'"';
            break;
        case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212:
            c = L'-';
            break;
        case 0x00A0: case 0x2002: case 0x2003: case 0x2009: case 0x200B: case 0x202F: case 0xFEFF:
            c = L' ';
            break;
        case 0x2026:
            w[n++] = L'.';
            w[n++] = L'.';
            c = L'.';
            break;
        default:
            if (c < 0x20 || c == 0x7F) {
                c = L' ';
            }
            break;
        }
        w[n++] = c;
        if (spellOut) {
            w[n++] = L' ';
        }
    }
    int cb = (n > 0) ? WideCharToMultiByte(CP_ACP, 0, w, (int)n, NULL, 0, NULL, NULL) : 0;
    if (cb < 0) {
        cb = 0;
    }
    char* out = (char*)malloc((size_t)cb + 1);
    if (out) {
        if (cb > 0) {
            WideCharToMultiByte(CP_ACP, 0, w, (int)n, out, cb, NULL, NULL);
        }
        out[cb] = '\0';
    }
    free(w);
    return out;
}

static bool HasContent(const char* s)
{
    for (; *s; ++s) {
        if (*s != ' ') {
            return true;
        }
    }
    return false;
}

// ---------- <pron> / SSML <phoneme> ----------
// SAPI delivers both as SPVA_Pronounce with the phonemes in State.pPhoneIds (often with
// empty text). LvCore kept AvCore's control tags: with synthText flags bit 1,
// \avprn=<word>=<transcription>\ registers a top-priority pronunciation for <word>, so a
// nonce word is registered and spoken. Ported from AcuVoiceRoger\src\AcuRogerSAPI.cpp.
static const char* const PRON_NONCE = "qzjx";
static const int         SYNTH_TAGS = 2;
static ISpPhoneConverter* volatile g_phoneConv = NULL;

// en-US converter in the classic SAPI alphabet that <pron sym="..."> uses.
static ISpPhoneConverter* GetPhoneConverter()
{
    if (g_phoneConv) {
        return g_phoneConv;
    }
    ISpPhoneConverter* pc = NULL;
    ISpObjectTokenCategory* cat = NULL;
    if (SUCCEEDED(CoCreateInstance(__uuidof(SpObjectTokenCategory), NULL, CLSCTX_ALL,
                                   __uuidof(ISpObjectTokenCategory), (void**)&cat))) {
        IEnumSpObjectTokens* en = NULL;
        if (SUCCEEDED(cat->SetId(SPCAT_PHONECONVERTERS, FALSE)) &&
            SUCCEEDED(cat->EnumTokens(L"Language=409", NULL, &en)) && en) {
            ISpObjectToken* tok = NULL;
            if (en->Next(1, &tok, NULL) == S_OK && tok) {
                tok->CreateInstance(NULL, CLSCTX_ALL, __uuidof(ISpPhoneConverter), (void**)&pc);
                tok->Release();
            }
            en->Release();
        }
        cat->Release();
    }
    if (pc && InterlockedCompareExchangePointer((PVOID volatile*)&g_phoneConv, pc, NULL) != NULL) {
        pc->Release();
    }
    return g_phoneConv;
}

struct PhoneMap {
    const char* sapi;
    const char* fonix;
    bool        vowel;
};

// aw/oy use the glide forms: the \avprn normalizer splits word-final V+V into two syllables.
static const PhoneMap PHONE_MAP[] = {
    {"iy", "e~", true}, {"ih", "i", true},  {"ey", "a~", true}, {"eh", "e", true},  {"ae", "a", true},
    {"aa", "o", true},  {"ao", "o", true},  {"ah", "u", true},  {"ax", "u", true},  {"uh", "u", true},
    {"uw", "u~", true}, {"er", "u|", true}, {"ay", "i~", true}, {"aw", "aw", true}, {"oy", "oy", true},
    {"ow", "o~", true},
    {"b", "b", false},  {"ch", "c~", false}, {"d", "d", false},  {"dh", "t~", false}, {"f", "f", false},
    {"g", "g", false},  {"hh", "h", false},  {"h", "h", false},  {"jh", "j", false},  {"k", "k", false},
    {"l", "l", false},  {"m", "m", false},   {"n", "n", false},  {"ng", "n~", false}, {"p", "p", false},
    {"r", "r", false},  {"s", "s", false},   {"sh", "s~", false}, {"t", "t", false},  {"th", "t'", false},
    {"v", "v", false},  {"w", "w", false},   {"y", "y", false},  {"z", "z", false},   {"zh", "z~", false},
};

static const PhoneMap* MapPhone(const char* s)
{
    for (size_t i = 0; i < sizeof(PHONE_MAP) / sizeof(PHONE_MAP[0]); ++i) {
        if (strcmp(PHONE_MAP[i].sapi, s) == 0) {
            return &PHONE_MAP[i];
        }
    }
    return NULL;
}

// SAPI phone ids -> " \avprn=qzjx=<transcription>\ qzjx ". NULL if nothing maps.
// The transcription never contains '\' (the tag terminator), so secondary stress is dropped;
// '/' follows the primary-stressed vowel only when another syllable follows it.
static char* BuildPronText(const SPPHONEID* ids)
{
    ISpPhoneConverter* pc = GetPhoneConverter();
    if (!pc || !ids || !*ids) {
        return NULL;
    }
    size_t count = wcslen(ids);
    WCHAR* sym = (WCHAR*)calloc(count * 8 + 1, sizeof(WCHAR));
    char* pron = (char*)malloc(count * 3 + 2);
    if (!sym || !pron || FAILED(pc->IdToPhone(ids, sym))) {
        free(sym);
        free(pron);
        return NULL;
    }
    size_t n = 0;
    int stressAt = -1;
    bool laterVowel = false;
    char tok[16];
    size_t tl = 0;
    for (size_t i = 0; ; ++i) {
        WCHAR w = sym[i];
        if (w != L' ' && w != 0) {
            if (tl < sizeof tok - 1) {
                tok[tl++] = (char)towlower(w);
            }
            continue;
        }
        tok[tl] = '\0';
        if (tl > 0) {
            if (tok[0] == '1') {
                stressAt = (int)n;
            } else if (tok[0] != '2') {
                const PhoneMap* m = MapPhone(tok);
                if (m) {
                    if (m->vowel && stressAt >= 0) {
                        laterVowel = true;
                    }
                    for (const char* p = m->fonix; *p; ++p) {
                        pron[n++] = *p;
                    }
                }
            }
        }
        tl = 0;
        if (w == 0) {
            break;
        }
    }
    pron[n] = '\0';
    free(sym);
    if (n == 0) {
        free(pron);
        return NULL;
    }
    if (stressAt > 0 && (size_t)stressAt < n && laterVowel) {
        memmove(pron + stressAt + 1, pron + stressAt, n - stressAt + 1);
        pron[stressAt] = '/';
    }
    size_t cap = strlen(pron) + 2 * strlen(PRON_NONCE) + 16;
    char* text = (char*)malloc(cap);
    if (text) {
        snprintf(text, cap, " \\avprn=%s=%s\\ %s ", PRON_NONCE, pron, PRON_NONCE);
    }
    free(pron);
    return text;
}

// ---------- Speak plumbing ----------
struct SpeakCtx {
    ISpTTSEngineSite* site;
    ULONGLONG         audioBytes;
    USHORT            volume;
    bool              aborted;
};

static void PollActions(SpeakCtx* ctx)
{
    DWORD actions = ctx->site->GetActions();
    if (actions & SPVES_ABORT) {
        ctx->aborted = true;
    }
    if (actions & SPVES_SKIP) {
        SPVSKIPTYPE type;
        long count;
        if (SUCCEEDED(ctx->site->GetSkipInfo(&type, &count))) {
            ctx->site->CompleteSkip(0);
        }
    }
    if (actions & SPVES_VOLUME) {
        ctx->site->GetVolume(&ctx->volume);
    }
    if (actions & SPVES_RATE) {
        long rate;
        ctx->site->GetRate(&rate);
    }
}

static HRESULT WritePcm(SpeakCtx* ctx, short* pcm, ULONG samples, ULONG fragVolume)
{
    const ULONG block = OUTPUT_RATE / 5;
    for (ULONG pos = 0; pos < samples; pos += block) {
        PollActions(ctx);
        if (ctx->aborted) {
            return S_OK;
        }
        ULONG n = (samples - pos < block) ? samples - pos : block;
        long scale = (long)ctx->volume * (long)(fragVolume > 100 ? 100 : fragVolume);
        if (scale < 10000) {
            for (ULONG i = pos; i < pos + n; ++i) {
                pcm[i] = (short)((long)pcm[i] * scale / 10000);
            }
        }
        ULONG written = 0;
        HRESULT hr = ctx->site->Write(pcm + pos, n * sizeof(short), &written);
        if (FAILED(hr)) {
            return hr;
        }
        ctx->audioBytes += n * sizeof(short);
    }
    return S_OK;
}

static HRESULT WriteSilence(SpeakCtx* ctx, long ms)
{
    if (ms <= 0) {
        return S_OK;
    }
    if (ms > 60000) {
        ms = 60000;
    }
    ULONG samples = (ULONG)((ULONGLONG)ms * OUTPUT_RATE / 1000);
    short* zeros = (short*)calloc(samples, sizeof(short));
    if (!zeros) {
        return E_OUTOFMEMORY;
    }
    HRESULT hr = WritePcm(ctx, zeros, samples, 100);
    free(zeros);
    return hr;
}

static HRESULT FireBookmark(SpeakCtx* ctx, const SPVTEXTFRAG* f)
{
    WCHAR* name = (WCHAR*)malloc(((size_t)f->ulTextLen + 1) * sizeof(WCHAR));
    if (!name) {
        return E_OUTOFMEMORY;
    }
    if (f->pTextStart && f->ulTextLen) {
        memcpy(name, f->pTextStart, (size_t)f->ulTextLen * sizeof(WCHAR));
    }
    name[f->pTextStart ? f->ulTextLen : 0] = L'\0';

    SPEVENT ev;
    memset(&ev, 0, sizeof ev);
    ev.eEventId             = SPEI_TTS_BOOKMARK;
    ev.elParamType          = SPET_LPARAM_IS_STRING;
    ev.ullAudioStreamOffset = ctx->audioBytes;
    ev.wParam               = (WPARAM)_wtol(name);
    ev.lParam               = (LPARAM)name;
    HRESULT hr = ctx->site->AddEvents(&ev, 1);
    free(name);
    return hr;
}

static bool IsSpoken(const SPVTEXTFRAG* f)
{
    SPVACTIONS a = f->State.eAction;
    return a == SPVA_Speak || a == SPVA_SpellOut || a == SPVA_Pronounce;
}

static bool AppendText(char** buf, size_t* len, size_t* cap, const char* s)
{
    size_t n = strlen(s);
    if (*len + n + 1 > *cap) {
        size_t ncap = (*cap ? *cap * 2 : 256);
        while (ncap < *len + n + 1) {
            ncap *= 2;
        }
        char* nb = (char*)realloc(*buf, ncap);
        if (!nb) {
            return false;
        }
        *buf = nb;
        *cap = ncap;
    }
    memcpy(*buf + *len, s, n + 1);
    *len += n;
    return true;
}

// Speaks [first, end) - consecutive spoken fragments with one volume - in a single synthText
// call, so tags such as <pron> or <emph> don't break the sentence into separately-phrased
// pieces. Phonemes win over a fragment's text (SAPI semantics); unmappable phonemes fall back
// to the text. A run without <pron> produces exactly the string the old per-fragment path did.
static HRESULT SpeakRun(SpeakCtx* ctx, const SPVTEXTFRAG* first, const SPVTEXTFRAG* end)
{
    char* buf = NULL;
    size_t len = 0, cap = 0;
    bool tags = false;
    const SPVTEXTFRAG* prev = NULL;
    HRESULT hr = S_OK;
    for (const SPVTEXTFRAG* f = first; f != end && SUCCEEDED(hr); f = f->pNext) {
        char* piece = NULL;
        bool isTag = false;
        if (f->State.eAction == SPVA_Pronounce) {
            piece = BuildPronText(f->State.pPhoneIds);
            isTag = (piece != NULL);
        }
        if (!piece) {
            if (!f->pTextStart || f->ulTextLen == 0) {
                continue;
            }
            piece = ToEngineText(f->pTextStart, f->ulTextLen, f->State.eAction == SPVA_SpellOut);
            if (!piece) {
                hr = E_OUTOFMEMORY;
                break;
            }
        }
        // Fragments that weren't adjacent in the source (a tag sat between them) get a word break.
        bool adjacent = prev && f->ulTextSrcOffset == prev->ulTextSrcOffset + prev->ulTextLen;
        if ((prev && !adjacent && !AppendText(&buf, &len, &cap, " ")) || !AppendText(&buf, &len, &cap, piece)) {
            hr = E_OUTOFMEMORY;
        }
        tags = tags || isTag;
        prev = f;
        free(piece);
    }
    // Under tag parsing a backslash in ordinary text would start a tag; only \avprn blocks may keep them.
    if (SUCCEEDED(hr) && tags) {
        for (char* p = buf; (p = strchr(p, '\\')) != NULL; ) {
            if (strncmp(p, "\\avprn=", 7) == 0) {
                char* close = strchr(p + 7, '\\');
                p = close ? close + 1 : p + 7;
            } else {
                *p++ = ' ';
            }
        }
    }
    if (FAILED(hr) || !buf) {
        free(buf);
        return hr;
    }
    short* pcm = NULL;
    ULONG samples = 0;
    hr = HasContent(buf) ? SynthToPcm(buf, tags ? SYNTH_TAGS : 0, &pcm, &samples) : S_FALSE;
    free(buf);
    if (hr == S_OK) {
        hr = WritePcm(ctx, pcm, samples, first->State.Volume);
        free(pcm);
    }
    return FAILED(hr) ? hr : S_OK;
}

// ---------- CJessicaEngine: ISpTTSEngine + ISpObjectWithToken ----------
class CJessicaEngine : public ISpTTSEngine, public ISpObjectWithToken
{
    LONG            m_cRef;
    ISpObjectToken* m_pToken;

public:
    CJessicaEngine() : m_cRef(1), m_pToken(NULL) {
        InterlockedIncrement(&g_cRefDll);
    }
    virtual ~CJessicaEngine() {
        if (m_pToken) m_pToken->Release();
        InterlockedDecrement(&g_cRefDll);
    }

    // ---- IUnknown ----
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ISpTTSEngine)) {
            *ppv = static_cast<ISpTTSEngine*>(this);
        } else if (IsEqualIID(riid, IID_ISpObjectWithToken)) {
            *ppv = static_cast<ISpObjectWithToken*>(this);
        } else {
            *ppv = NULL;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef()  { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() {
        LONG c = InterlockedDecrement(&m_cRef);
        if (c == 0) delete this;
        return c;
    }

    // ---- ISpObjectWithToken ----
    STDMETHODIMP SetObjectToken(ISpObjectToken* pToken) {
        if (m_pToken) { m_pToken->Release(); m_pToken = NULL; }
        if (pToken) { pToken->AddRef(); m_pToken = pToken; }
        return S_OK;
    }
    STDMETHODIMP GetObjectToken(ISpObjectToken** ppToken) {
        if (!ppToken) return E_POINTER;
        *ppToken = m_pToken;
        if (m_pToken) m_pToken->AddRef();
        return S_OK;
    }

    // ---- ISpTTSEngine ----
    STDMETHODIMP Speak(DWORD dwSpeakFlags,
                       REFGUID rguidFormatId,
                       const WAVEFORMATEX* pWaveFormatEx,
                       const SPVTEXTFRAG* pTextFragList,
                       ISpTTSEngineSite* pOutputSite);

    STDMETHODIMP GetOutputFormat(const GUID* pTargetFormatId,
                                 const WAVEFORMATEX* pTargetWaveFormatEx,
                                 GUID* pOutputFormatId,
                                 WAVEFORMATEX** ppCoMemOutputWaveFormatEx) {
        if (!pOutputFormatId || !ppCoMemOutputWaveFormatEx) return E_POINTER;
        *pOutputFormatId = SPDFID_WaveFormatEx;
        WAVEFORMATEX* w = (WAVEFORMATEX*)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
        if (!w) return E_OUTOFMEMORY;
        w->wFormatTag      = WAVE_FORMAT_PCM;
        w->nChannels       = OUTPUT_CHANNELS;
        w->nSamplesPerSec  = OUTPUT_RATE;
        w->wBitsPerSample  = OUTPUT_BITS;
        w->nBlockAlign     = (w->nChannels * w->wBitsPerSample) / 8;
        w->nAvgBytesPerSec = w->nSamplesPerSec * w->nBlockAlign;
        w->cbSize          = 0;
        *ppCoMemOutputWaveFormatEx = w;
        return S_OK;
    }
};

STDMETHODIMP CJessicaEngine::Speak(DWORD /*dwSpeakFlags*/,
                                   REFGUID /*rguidFormatId*/,
                                   const WAVEFORMATEX* /*pWaveFormatEx*/,
                                   const SPVTEXTFRAG* pTextFragList,
                                   ISpTTSEngineSite* pOutputSite)
{
    if (!pOutputSite) {
        return E_POINTER;
    }

    SpeakCtx ctx;
    ctx.site       = pOutputSite;
    ctx.audioBytes = 0;
    ctx.volume     = 100;
    ctx.aborted    = false;
    pOutputSite->GetVolume(&ctx.volume);

    for (const SPVTEXTFRAG* f = pTextFragList; f; ) {
        PollActions(&ctx);
        if (ctx.aborted) {
            break;
        }
        if (IsSpoken(f)) {
            const SPVTEXTFRAG* end = f->pNext;
            while (end && IsSpoken(end) && end->State.Volume == f->State.Volume) {
                end = end->pNext;
            }
            HRESULT hr = SpeakRun(&ctx, f, end);
            if (FAILED(hr)) {
                return hr;
            }
            f = end;
            continue;
        }
        HRESULT hr = S_OK;
        switch (f->State.eAction) {
        case SPVA_Silence:
            hr = WriteSilence(&ctx, f->State.SilenceMSecs);
            break;
        case SPVA_Bookmark:
            hr = FireBookmark(&ctx, f);
            break;
        default:
            break;
        }
        if (FAILED(hr)) {
            return hr;
        }
        f = f->pNext;
    }
    return S_OK;
}

// ---------- Class factory ----------
class CJessicaEngineFactory : public IClassFactory
{
    LONG m_cRef;
public:
    CJessicaEngineFactory() : m_cRef(1) {}
    virtual ~CJessicaEngineFactory() {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef()  { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() {
        LONG c = InterlockedDecrement(&m_cRef);
        if (c == 0) delete this;
        return c;
    }
    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) {
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;
        if (!ppv) return E_POINTER;
        *ppv = NULL;
        CJessicaEngine* p = new CJessicaEngine();
        if (!p) return E_OUTOFMEMORY;
        HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL bLock) {
        if (bLock) InterlockedIncrement(&g_cRefDll);
        else       InterlockedDecrement(&g_cRefDll);
        return S_OK;
    }
};

// ---------- DLL exports ----------
extern "C" {

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD dwReason, LPVOID /*lpReserved*/)
{
    if (dwReason == DLL_PROCESS_ATTACH) {
        g_hThisDll = hInst;
        DisableThreadLibraryCalls(hInst);
        InitializeCriticalSection(&g_csEngine);
        GetModuleFileNameA(hInst, g_thisDllPath, MAX_PATH);
        ComputePaths();
        InitHalfband();
    } else if (dwReason == DLL_PROCESS_DETACH) {
        DeleteCriticalSection(&g_csEngine);
    }
    return TRUE;
}

// LvCore is never torn down (closeSoundbank is not trusted), so once it is loaded this DLL stays put.
STDAPI DllCanUnloadNow()
{
    return (g_cRefDll == 0 && g_hLvCore == NULL) ? S_OK : S_FALSE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (!IsEqualCLSID(rclsid, CLSID_FonixJessica)) return CLASS_E_CLASSNOTAVAILABLE;
    CJessicaEngineFactory* f = new CJessicaEngineFactory();
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}

// ---------- Self-registration (32-bit regsvr32 lands these in WOW6432Node) ----------
static HRESULT WriteRegStr(HKEY hKey, LPCSTR sub, LPCSTR name, LPCSTR val)
{
    HKEY h;
    LONG r = RegCreateKeyExA(hKey, sub, 0, NULL, 0, KEY_WRITE, NULL, &h, NULL);
    if (r != ERROR_SUCCESS) return HRESULT_FROM_WIN32(r);
    r = RegSetValueExA(h, name, 0, REG_SZ, (const BYTE*)val, (DWORD)strlen(val) + 1);
    RegCloseKey(h);
    return (r == ERROR_SUCCESS) ? S_OK : HRESULT_FROM_WIN32(r);
}

STDAPI DllRegisterServer()
{
    char buf[256];

    snprintf(buf, sizeof buf, "CLSID\\%s", CLSID_STR);
    if (FAILED(WriteRegStr(HKEY_CLASSES_ROOT, buf, NULL, "Fonix Jessica TTS Engine"))) return E_FAIL;
    snprintf(buf, sizeof buf, "CLSID\\%s\\InprocServer32", CLSID_STR);
    if (FAILED(WriteRegStr(HKEY_CLASSES_ROOT, buf, NULL, g_thisDllPath))) return E_FAIL;
    if (FAILED(WriteRegStr(HKEY_CLASSES_ROOT, buf, "ThreadingModel", "Both"))) return E_FAIL;

    if (FAILED(WriteRegStr(HKEY_LOCAL_MACHINE, TOKEN_KEY, NULL, VOICE_NAME))) return E_FAIL;
    if (FAILED(WriteRegStr(HKEY_LOCAL_MACHINE, TOKEN_KEY, "CLSID", CLSID_STR))) return E_FAIL;
    if (FAILED(WriteRegStr(HKEY_LOCAL_MACHINE, TOKEN_KEY, "LangDataPath", ""))) return E_FAIL;
    if (FAILED(WriteRegStr(HKEY_LOCAL_MACHINE, TOKEN_KEY, "VoicePath", ""))) return E_FAIL;
    if (FAILED(WriteRegStr(HKEY_LOCAL_MACHINE, TOKEN_KEY, "409", VOICE_NAME))) return E_FAIL;

    snprintf(buf, sizeof buf, "%s\\Attributes", TOKEN_KEY);
    if (FAILED(WriteRegStr(HKEY_LOCAL_MACHINE, buf, "Name", VOICE_NAME))) return E_FAIL;
    if (FAILED(WriteRegStr(HKEY_LOCAL_MACHINE, buf, "Gender", "Female"))) return E_FAIL;
    if (FAILED(WriteRegStr(HKEY_LOCAL_MACHINE, buf, "Age", "Adult"))) return E_FAIL;
    if (FAILED(WriteRegStr(HKEY_LOCAL_MACHINE, buf, "Language", "409"))) return E_FAIL;
    if (FAILED(WriteRegStr(HKEY_LOCAL_MACHINE, buf, "Vendor", "Fonix"))) return E_FAIL;
    if (FAILED(WriteRegStr(HKEY_LOCAL_MACHINE, buf, "Version", "1.0"))) return E_FAIL;
    return S_OK;
}

STDAPI DllUnregisterServer()
{
    char buf[256];
    snprintf(buf, sizeof buf, "CLSID\\%s\\InprocServer32", CLSID_STR);
    RegDeleteKeyA(HKEY_CLASSES_ROOT, buf);
    snprintf(buf, sizeof buf, "CLSID\\%s", CLSID_STR);
    RegDeleteKeyA(HKEY_CLASSES_ROOT, buf);
    snprintf(buf, sizeof buf, "%s\\Attributes", TOKEN_KEY);
    RegDeleteKeyA(HKEY_LOCAL_MACHINE, buf);
    RegDeleteKeyA(HKEY_LOCAL_MACHINE, TOKEN_KEY);
    return S_OK;
}

} // extern "C"
