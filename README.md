# Fonix Jessica SAPI5 voice

A SAPI5 voice that lets Windows apps speak with the Fonix "Jessica" Pocket PC TTS voice. It runs through the desktop FAAST v520 engine (`LvCore.dll`), with a reverse-engineered replacement for that engine's Cybit sound bank decoder. Sister project to [Fonix Roger](https://github.com/wagwan-piffting-blud/Fonix-Roger). This README covers installation, usage, tuning, and how the port was put together.

## What you get

- A SAPI5 voice token named **"Fonix Jessica"** (female, US English).
- Usable from any 32-bit SAPI5 client: Balabolka, NVDA, TTSApp, and 32-bit PowerShell `SAPI.SpVoice`.
- Audio: 16 kHz mono 16-bit PCM. The sound bank itself is 8 kHz; the engine upsamples 2x with a half-band FIR filter, and every other output sample is bit-identical to the engine's 8 kHz render.
- A brighter decode than stock LvCore gives this sound bank. It adds the Pocket PC engine's per-unit fades, a stronger spectral tilt, and makeup gain (see [Sound: the Cybit decoder](#sound-the-cybit-decoder)). All three are adjustable through registry values.
- SAPI volume works (including XML `<volume>`), as do `<silence>` and `<spell>`. `<bookmark>` tags are reported as bookmark events instead of being read aloud.
- Custom pronunciations work: SAPI XML `<pron sym="p aa 1 t ax w aa t uw m iy"/>` and SSML `<phoneme alphabet="x-microsoft-sapi" ph="...">` speak the given SAPI phonemes (see [Pronunciations](#pronunciations)). SSML is otherwise parsed by SAPI itself and needs nothing from the voice.

## Install

1. Extract this folder anywhere.
2. Double-click `RUN_ME.cmd` and accept the UAC prompt.
3. Wait a few seconds for the files to copy and the voice to register.
4. Done. No reboot needed; restart any SAPI app that was already open so it picks up the new voice.

Files install to `C:\Program Files (x86)\FonixJessicaSAPI\`.

For an unattended install from an elevated prompt:

```powershell
powershell -ExecutionPolicy Bypass -File install.ps1 -NoPause
```

## Quick test

Paste into PowerShell:

```powershell
& "C:\Windows\SysWOW64\WindowsPowerShell\v1.0\powershell.exe" -Command '$v = New-Object -ComObject SAPI.SpVoice; $v.Voice = $v.GetVoices(''Name=Fonix Jessica'').Item(0); $v.Speak(''Hello from Jessica.'')'
```

Or pick "Fonix Jessica" in the voice list of Balabolka, TTSApp, NVDA, or your frontend of choice. `demo.wav` is a sample rendered through Balabolka's `balcon` CLI.

## Uninstall

```powershell
powershell -ExecutionPolicy Bypass -File "C:\Program Files (x86)\FonixJessicaSAPI\uninstall.ps1"
```

Unregisters the COM DLL, removes the voice token, and deletes the install folder. Other voices (including Fonix Roger) are untouched.

## Tuning

The decoder reads three optional DWORD values from the voice token. If a value isn't set, its default applies. Set them from an elevated PowerShell:

```powershell
$k = "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Speech\Voices\Tokens\FonixJessica"
New-ItemProperty $k -Name CybitTilt -PropertyType DWord -Value -2867 -Force
New-ItemProperty $k -Name CybitGain -PropertyType DWord -Value 8192 -Force
New-ItemProperty $k -Name CybitFade -PropertyType DWord -Value 1 -Force
```

| Value | Default | Meaning |
|---|---|---|
| `CybitTilt` | `-2867` | Postfilter tilt coefficient, Q12 (`-4096` = -1.0). More negative is brighter. LvCore's stock value is `-1024`. |
| `CybitGain` | `8192` | Output gain, Q12 (`4096` = 1x, `8192` = 2x, max `32767`). Saturates instead of wrapping. |
| `CybitFade` | `1` | `1` = 64-sample fade in/out on each sound unit, as on the Pocket PC. `0` = no fades, as in desktop LvCore. |

- `CybitTilt=-1024`, `CybitGain=4096`, `CybitFade=0` reproduces stock LvCore output bit for bit.
- Raising the gain or making the tilt more negative can clip loud passages.
- The values are read once per process, when the voice first loads, so restart the SAPI app after a change.
- Uninstalling or reinstalling deletes the voice token along with these values.

To return to the defaults:

```powershell
Remove-ItemProperty "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Speech\Voices\Tokens\FonixJessica" -Name CybitTilt, CybitGain, CybitFade -ErrorAction SilentlyContinue
```

## Pronunciations

Use the classic SAPI American English phoneme set, with `1` after the primary-stressed vowel:

```xml
Test of Fonix Jessica. <pron sym="p aa 1 t ax w aa t uw m iy"/>
```

- **How it works.** SAPI passes the phonemes to the engine as phone IDs. The engine converts them to Fonix's own transcription and hands that to LvCore through the `\avprn=<word>=<transcription>\` control tag, a feature Fonix inherited from AcuVoice. The tag registers a throwaway word with that pronunciation, and the engine then speaks that word.
- **Ported code.** The phone map comes from the AcuVoice Roger port.
- **Plain text is unaffected.** LvCore's tag parsing is only switched on for text that contains a pronunciation, and backslashes in the surrounding text are blanked first, so ordinary text renders exactly as before.
- **Seamless.** SAPI splits text into fragments at every tag. The engine joins consecutive spoken fragments that share a volume back into one `synthText` call, so a `<pron>`, `<emph>`, or similar tag in mid-sentence doesn't add pauses or reset the intonation. `<silence>`, `<bookmark>`, and volume changes still split the sentence at the tag.
- **Limitations.** Secondary stress (`2`) is dropped. A `<pron>` with text inside speaks the phonemes, not the text. Phonemes with no mapping are skipped. If none of the phonemes map, the text inside the tag is spoken instead.
- **Clients must parse the tag.** The client has to hand the text to SAPI as XML. A client that sends the text as plain text gets the tag read aloud literally.

## Known limitations

- **32-bit clients only.** `LvCore.dll` is 32-bit, so the engine is too. The voice token lives under `HKLM\SOFTWARE\WOW6432Node\Microsoft\Speech\Voices\Tokens\FonixJessica`, which 64-bit SAPI clients (Windows Narrator, Edge Read Aloud, 64-bit PowerShell) don't see.
- **Rate and pitch are ignored.** SAPI rate/pitch settings and XML `<rate>`/`<pitch>` are accepted but have no effect. Volume works.
- **No word or sentence boundary events.** Clients that highlight words as they're spoken (e.g. Balabolka) won't highlight with this voice.
- **Telephone bandwidth.** The sound bank is 8 kHz audio coded at 2 kbps. The 16 kHz output format doesn't add high frequencies, and tuning can't restore detail the codec never stored.

## Sound: the Cybit decoder

Jessica's sound bank is Cybit64, a 2 kbps LPC vocoder.

- **Frames.** Each sound unit is a run of 40-bit frames: 7-bit pitch, 23-bit split-VQ line spectral frequencies, 6-bit gain, and 4-bit voicing.
- **Synthesis.** Each frame produces 160 samples through a mixed pulse/noise excitation and an LPC postfilter.

### Stock LvCore vs. the Pocket PC engine

Stock LvCore decodes the bank correctly, but the result sounds dull and hollow; the long-term average spectrum has a centroid around 330 Hz. To check whether that was a desktop bug, the decoder was reversed in Ghidra from two engines:

- LvCore v520 (x86).
- The Pocket PC engine, `FonixTTSuvCE.dll` (ARM).

Both use identical tables and synthesis code. The only difference is in the unit dispatcher: the Pocket PC engine fades every unit longer than 128 samples in and out over 64 samples, and desktop LvCore doesn't. So the dull tone is authentic to the codec, not a desktop bug.

### The replacement decoder

`src\cybit.c` reimplements the decoder in C. Its stock preset matches LvCore bit for bit on all 8 test sentences and 55 raw units. When the engine loads `LvCore.dll`, it checks the CALL to the Cybit decoder at RVA `0xD846` and redirects it to `cybit.c`. An unrecognized LvCore build keeps the stock decoder.

The engine's defaults:

1. **Pocket PC unit fades.** Restores the device's behavior.
2. **Tilt -0.7** in the postfilter's tilt stage, in place of LvCore's -0.25. This moves the spectral centroid to about 490 Hz. For comparison, a natural female voice at the same sample rate (AT&T Natural Voices Crystal, resampled to 8 kHz) measures 474 Hz.
3. **2x output gain.** Makes up the roughly 6 dB the tilt removes. Speech RMS lands between -17 and -18 dBFS, the same as stock, with no clipping on the test set.

The postfilter gammas and the voicing mix stay stock; changing them made no audible difference.

## How the port works

The Jessica cab in the Disk1 installer (`US Jessica (4 MB).PPC300_2577.cab`) predates the v600 cab the Roger port started from. It already contains the v500 formats that the desktop engine reads natively:

- `hashsnds.ply` and `hashfon1-4.cmp`: the Cybit sound bank.
- Text `*.tab` and `*.dvl` files: the dictionary hash tables.

No sound bank or hash conversion was needed. The port has five parts:

1. **Staging.** `tools\stage_jessica.py` parses the cab's `USJESSIC.000` setup file (MSCE format). It maps each numbered cab member (`hashfon1.010`, `0000dict.017`, ...) back to its real name and install directory and stages it under `data\`. Jessica's own prosody tables (`jprtable.srt`, `jptableex.srt`) become `prtable.srt` and `ptableex.srt`; the Roger tables in the same cab are skipped. The empty custom-dictionary stubs and the three engine DLLs come from the Roger package. `LvCore.dll` carries the Roger port's license patch (6 NOPs at file offset `0x17C4B`).
2. **Dictionary.** The main dictionary has 288 `[` bytes across 152 phoneme records (hello, eight, morning, thousand, ...). In the v600 dictionary Roger used, `[` crashed v520 synthesis. Jessica's older dictionary uses it differently, and all 152 words synthesize cleanly (`tools\dict_brackets.py` finds them, `tools\crash_scan.py` tests them), so the dictionary ships unpatched.
3. **Engine shim.** `src\JessicaSAPI.cpp` is the `ISpTTSEngine` shim over LvCore's `synthText()`. It loads `lib\LvCore.dll` relative to its own location and serializes engine access. If the engine faults, it skips that text fragment and reopens the sound bank.
4. **Pronunciations.** `<pron>`/`<phoneme>` fragments become `\avprn=` control tags, synthesized with `synthText` flags bit 1 set (see [Pronunciations](#pronunciations)).
5. **Decoder.** `src\cybit.c` replaces LvCore's Cybit decoder at load time, as described above. `tools\gen_cybit_tables.py` extracts the codebooks and filter tables from `lib\LvCore.dll` into `src\cybit_tables.h`.

## Rebuilding from source

Requirements:

- Python 3 with `tqdm`.
- Visual Studio 2022 with the C++ x86 toolset.
- A Windows 10/11 SDK.

`stage_jessica.py` expects the original installer files under `import\Disk1\` and the Fonix Roger package at `FonixRogerSAPI_pkg`.

1. `python tools\stage_jessica.py` stages `data\` and `lib\`.
2. `python tools\gen_cybit_tables.py` regenerates `src\cybit_tables.h`. It refuses any `LvCore.dll` other than the patched v520 build.
3. `build.bat` builds `JessicaSAPI.dll` into the repo root.
4. `tools\build_tools.bat` builds `tools\fonix_synth.exe` (direct LvCore harness) and `tools\sapi_probe.exe` (renders installed SAPI voices to WAV from a script file).

The harness uses the stock decoder unless `FONIX_CYBIT` is set:

- `FONIX_CYBIT=ppc,tilt=-2867,gain=0x2000` renders exactly what the SAPI voice produces at 8 kHz.
- `FONIX_CYBIT=v520` renders stock output through `cybit.c`.
- `FONIX_FLAGS=2` passes `synthText` flags 2, enabling `\avprn=` tags in the input text.

## Files

```
JessicaSAPI.dll               the SAPI5 engine shim
lib\                          LvCore.dll, FTKcUtils.dll, MemLib.dll (FAAST v520 runtime)
data\Jessica\cb112298\        Jessica sound bank (Cybit64, 8 kHz)
data\dictfls\                 dictionaries, hash tables, prosody tables
data\userdict\                user dictionary
install.ps1, uninstall.ps1    installer scripts (RUN_ME.cmd wraps install.ps1)
demo.wav                      sample output
build.bat                     engine build script
src\JessicaSAPI.cpp           SAPI5 engine source
src\cybit.c, src\cybit.h      Cybit64 decoder reimplementation and LvCore hook addresses
src\cybit_tables.h            decoder tables extracted from LvCore.dll
tools\                        staging, table extraction, dictionary, and test tools
```

## Removing manually if the uninstaller breaks

Delete:
- `C:\Program Files (x86)\FonixJessicaSAPI\` (folder)
- `HKLM\SOFTWARE\WOW6432Node\Microsoft\Speech\Voices\Tokens\FonixJessica` (registry key)
- `HKLM\SOFTWARE\Classes\WOW6432Node\CLSID\{B3478C8E-CA57-4152-B62E-2CAC971CEC7F}` (registry key)

## License / credits

Most of the SAPI shim, the Cybit decoder reimplementation, and the tooling were written by Claude Code, building on the reverse engineering done for the Fonix Roger port. The original voice files come from a Fonix Pocket PC voice installer. This project is provided as-is, with no warranty, and is intended for educational purposes only. The Jessica voice data's copyright status is unknown but it is likely still owned by Fonix in some regard. The SAPI shim code and the rest of this repository's generated code are licensed under GNU GPL v3.0; see the LICENSE file.

Project spearheaded by Wags (https://wagspuzzle.space/), with help from Claude Code (https://claude.ai/) for the SAPI shim, reverse engineering, and overall project structure.

## GenAI Disclosure Notice: Portions of this repository have been generated using Generative AI tools (Claude, Claude Code).
