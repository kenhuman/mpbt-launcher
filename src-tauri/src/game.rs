/// game.rs — Game launch with DirectDraw shim deployment + multi-instance patch
///
/// All launcher-managed display modes deploy `ddraw.dll` and write a matching
/// `ddraw.ini` next to the game EXE so the launcher owns scaling and pacing.
///
/// For the dedicated `fullscreen` option we keep the stock EXE path, but still
/// configure the shim for native fullscreen handling.
///
/// All other display modes use a version-keyed patched copy of the EXE
/// (`<stem>_windowed_<source-fingerprint>.exe`) created alongside the original.
/// Two patches are applied to the copy:
///   • the single-instance guard (JZ → JMP, one byte) so multiple simultaneous
///     instances are allowed
///   • the self-integrity CRC check bypass, because patching the guard changes
///     the file's CRC and would otherwise cause a startup failure
///
/// Using a separate file avoids trying to write to an EXE that Windows has
/// memory-mapped as a running process (which would fail with a sharing
/// violation).
///
/// Embedding the DLL bytes in the binary avoids Windows Defender quarantining a
/// standalone ddraw.dll sitting in the build tree.

/// DLL bytes baked in at compile time.  `native/build.bat` must have been run
/// before `cargo build` / `tauri dev`.
#[cfg(target_os = "windows")]
static DDRAW_DLL_BYTES: &[u8] = include_bytes!("../../native/ddraw.dll");

/// Windows ERROR_SHARING_VIOLATION — the file is open by another process.
#[cfg(target_os = "windows")]
const ERROR_SHARING_VIOLATION: i32 = 32;

/// Return the path of the windowed-mode EXE copy, creating it if needed.
///
/// The copy is placed next to the original as
/// `<stem>_windowed_<source-fingerprint>.exe`.
///
/// Keying the sidecar to the original EXE bytes avoids reusing a stale patched
/// copy after the client is upgraded in place (for example `v1.23` -> `v1.29`
/// under the same `C:\MPBT` install path).
///
/// The single-instance guard is a single `JZ` byte (`0x74`) changed to `JMP`
/// (`0xEB`).  We locate it by scanning for the distinctive surrounding bytes:
///
///   TEST EAX,EAX; Jcc +0x15; PUSH 1; PUSH EAX
///   85 C0 [74|EB] 15 6A 01 50
///           ^--- patch target (index 2)
#[cfg(target_os = "windows")]
fn source_fingerprint(bytes: &[u8]) -> u64 {
    const FNV_OFFSET: u64 = 0xcbf29ce484222325;
    const FNV_PRIME: u64 = 0x100000001b3;

    let mut hash = FNV_OFFSET;
    for byte in bytes {
        hash ^= u64::from(*byte);
        hash = hash.wrapping_mul(FNV_PRIME);
    }
    hash
}

#[cfg(target_os = "windows")]
fn windowed_exe(original: &std::path::Path) -> Result<std::path::PathBuf, String> {
    let mut data = std::fs::read(original).map_err(|e| format!("Failed to read game EXE: {e}"))?;
    let exe_fingerprint = source_fingerprint(&data);

    let stem = original
        .file_stem()
        .ok_or("game_exe has no file stem")?
        .to_string_lossy();
    let patched = original.with_file_name(format!(
        "{stem}_windowed_{exe_fingerprint:016x}.exe"
    ));

    const PATCH_IDX: usize = 2;
    const BYTE_JZ: u8 = 0x74;
    const BYTE_JMP: u8 = 0xEB;

    // Patch 1: single-instance guard (JZ → JMP).
    // Pattern: TEST EAX,EAX; Jcc +0x15; PUSH 1; PUSH EAX
    //          85 C0 [74|EB] 15 6A 01 50
    let found = data.windows(7).enumerate().find(|(_, w)| {
        w[0] == 0x85
            && w[1] == 0xC0
            && (w[PATCH_IDX] == BYTE_JZ || w[PATCH_IDX] == BYTE_JMP)
            && w[3] == 0x15
            && w[4] == 0x6A
            && w[5] == 0x01
            && w[6] == 0x50
    });
    let (single_instance_offset, _) = found.ok_or_else(|| {
        format!(
            "Unsupported game EXE ({}): multi-instance signature not found",
            original.display()
        )
    })?;
    data[single_instance_offset + PATCH_IDX] = BYTE_JMP;

    // Patch 2: self-integrity CRC check bypass.
    // The game calls GetModuleFileNameA on itself, computes a CRC, and
    // compares it against a stored expected value.  Since we changed a byte
    // in this copy the CRC will never match — bypassing the check is the
    // only practical option.
    //
    // The check ends with: SUB EAX,0xa; CMP EAX,0x1; SBB EAX,EAX; NEG EAX; RET
    // (returns 1 iff original return value was exactly 10 = CRC match)
    // We replace the whole sequence with: MOV EAX,1; NOP×5; RET
    // SBB EAX,EAX is encoded as 0x19 0xC0 (v1.06) or 0x1B 0xC0 (v1.23).
    const CRC_PATTERNS: [[u8; 11]; 2] = [
        [
            0x83, 0xE8, 0x0A, 0x83, 0xF8, 0x01, 0x19, 0xC0, 0xF7, 0xD8, 0xC3,
        ],
        [
            0x83, 0xE8, 0x0A, 0x83, 0xF8, 0x01, 0x1B, 0xC0, 0xF7, 0xD8, 0xC3,
        ],
    ];
    const CRC_PATCH: [u8; 11] = [
        0xB8, 0x01, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90, 0xC3,
    ];
    let crc_offset = data
        .windows(11)
        .enumerate()
        .find_map(|(offset, window)| {
            if CRC_PATTERNS.iter().any(|pattern| window == pattern) || window == CRC_PATCH {
                Some(offset)
            } else {
                None
            }
        })
        .ok_or_else(|| {
            format!(
                "Unsupported game EXE ({}): CRC bypass signature not found",
                original.display()
            )
        })?;
    data[crc_offset..crc_offset + 11].copy_from_slice(&CRC_PATCH);

    // Use create_new (O_CREAT | O_EXCL) so only one process wins the race.
    // If another launcher got here first, AlreadyExists means the copy is ready
    // and we can use it as-is.
    use std::io::Write as _;
    match std::fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&patched)
    {
        Ok(mut f) => f
            .write_all(&data)
            .map_err(|e| format!("Failed to write windowed EXE copy: {e}"))?,
        Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => { /* another process created it */
        }
        Err(e) => return Err(format!("Failed to create windowed EXE copy: {e}")),
    }

    Ok(patched)
}

/// Return the `[display]` section content for `ddraw.ini` given a display mode
/// string.
fn ddraw_ini_content(display_mode: &str) -> String {
    const FPS_LIMIT: u32 = 60;

    match display_mode {
        "fullscreen" => {
            format!("[display]\nmode=fullscreen-native\nfps_limit={FPS_LIMIT}\n")
        }
        "window-fullscreen" => {
            format!("[display]\nmode=fullscreen-window\nfps_limit={FPS_LIMIT}\n")
        }
        other => {
            // Expect "window-WxH" e.g. "window-1920x1080"
            if let Some(size) = other.strip_prefix("window-") {
                if let Some((w, h)) = size.split_once('x') {
                    if let (Ok(w), Ok(h)) = (w.parse::<u32>(), h.parse::<u32>()) {
                        return format!(
                            "[display]\nwidth={w}\nheight={h}\nfps_limit={FPS_LIMIT}\n"
                        );
                    }
                }
            }
            // Unknown or malformed windowed mode — use plain windowed at game resolution
            format!("[display]\nfps_limit={FPS_LIMIT}\n")
        }
    }
}

#[cfg(target_os = "windows")]
pub fn launch_game(
    game_exe: &std::path::Path,
    display_mode: &str,
    pcgi_path: &std::path::Path,
) -> Result<(), String> {
    let game_dir = game_exe
        .parent()
        .ok_or("game_exe has no parent directory")?;
    let dest_ddraw = game_dir.join("ddraw.dll");
    let dest_ini = game_dir.join("ddraw.ini");

    // All launcher display modes now go through the shim so the launcher can
    // own display behavior and the 60 FPS pacing fix consistently.
    let ini_content = ddraw_ini_content(display_mode);

    // Write ddraw.ini (mode/resolution config for the shim).
    std::fs::write(&dest_ini, &ini_content).map_err(|e| format!("Failed to write ddraw.ini: {e}"))?;

    // Write the ddraw shim. Sharing violation is non-fatal — another instance
    // already placed it.
    if let Err(e) = std::fs::write(&dest_ddraw, DDRAW_DLL_BYTES) {
        if e.raw_os_error() != Some(ERROR_SHARING_VIOLATION) {
            return Err(format!("Failed to write ddraw.dll to game dir: {e}"));
        }
    }

    let exe_to_launch = if display_mode == "fullscreen" {
        // Keep the stock EXE name/launch path for native fullscreen mode.
        game_exe.to_path_buf()
    } else {
        // Use the patched copy for shim-managed windowed modes so we never
        // touch a running EXE and still allow multi-instance launches.
        windowed_exe(game_exe)?
    };

    std::process::Command::new(&exe_to_launch)
        .arg(pcgi_path)
        .current_dir(game_dir)
        .spawn()
        .map_err(|e| format!("Failed to launch game: {e}"))?;

    Ok(())
}

#[cfg(not(target_os = "windows"))]
pub fn launch_game(
    _game_exe: &std::path::Path,
    _display_mode: &str,
    _pcgi_path: &std::path::Path,
) -> Result<(), String> {
    Err("MPBT only runs on Windows".to_string())
}
