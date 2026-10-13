# `lsfg-shader-library` test suite

Host unit test suite for the runtime LSFG shader library (`retoque/lsfg/LsfgShaderLibrary.{h,cpp}` and `retoque/lsfg/LsfgEnvironment.h`).

## What it covers

1. **Discovery & Path Resolution:**
   - **Windows Registry & Steam Libraries:** Parsing `HKCU\Software\Valve\Steam\SteamPath` and reading multi-library configurations from `<Steam>\steamapps\libraryfolders.vdf`.
   - **Wine / Proton `Z:` Drive Mapping:** Scanning Linux Steam paths via `Z:\home\<user>\...`, `$HOME`, and `$XDG_DATA_HOME`.
   - **INI / Config Override:** Explicit path override taking precedence.
   - **Lossless.dll and Folder Mapping:** Automatically resolving directory paths and `Lossless.dll` / `LosslessScaling.dll` to `lsfg-vk.dll` beside them.
2. **Resource ID Resolution:**
   - Base shaders: `mipmaps` (0), `generate_8bit` (1), `generate_16bit` (2).
   - Core shaders (24 shaders): `rid = base_idx + (fp16 ? 48 : 0) + (performance ? 24 : 0)`.
3. **PE Resource Parsing:**
   - Parsing the Portable Executable (PE32+ / PE32) resource directory tree (`RT_RCDATA`, type 10).
   - Resolving both integer resource IDs and string-encoded IDs (such as `"0"` / `0x80001348` mapping to resource 0).
   - Validating SPIR-V magic word (`0x07230203`) and 4-byte word alignment.
4. **Content-Hash-Keyed Cache:**
   - SHA-256 computation on DLL content.
   - Invalidation when the DLL content changes.
   - Storing, loading, and checking existence of DXIL blobs by resource ID.
5. **Real DLL Test:**
   - When the user's actual `lsfg-vk.dll` is present at `~/.local/share/Steam/steamapps/common/Lossless Scaling/lsfg-vk.dll`, extracts and verifies all 99 modules (0..98) and content hash. Skipped gracefully if absent; never extracts or commits proprietary content.

## What it does not cover

- Offline conversion from SPIR-V to HLSL/DXIL (handled by tools/build scripts).
- D3D12 device execution (covered by D3D12 test harness).

## Running

```bash
python3 tests/lsfg-shader-library/run.py
```
