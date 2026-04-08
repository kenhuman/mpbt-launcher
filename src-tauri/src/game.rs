/// game.rs — Game launch with optional DirectDraw windowed shim + multi-instance patch
///
/// When `windowed` is true:
///   • The ddraw.dll shim is written into the game directory so the game runs
///     in a window (Windows DLL search order picks it up before system ddraw).
///   • A permanently-patched copy of the EXE (`<stem>_windowed.exe`) is created
///     once alongside the original.  The patched copy has the single-instance
///     guard removed (JZ → JMP, one byte) so multiple simultaneous instances
///     are allowed.  The original is never modified.
///   • The patched copy is what gets launched.
///
/// When `windowed` is false the ddraw shim is removed and the original
/// unpatched EXE is launched as normal.
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
/// The copy is placed next to the original as `<stem>_windowed.exe`
/// (e.g. `Mpbtwin.exe` → `Mpbtwin_windowed.exe`).
///
/// The single-instance guard is a single `JZ` byte (`0x74`) changed to `JMP`
/// (`0xEB`).  We locate it by scanning for the distinctive surrounding bytes:
///
///   TEST EAX,EAX; Jcc +0x15; PUSH 1; PUSH EAX
///   85 C0 [74|EB] 15 6A 01 50
///           ^--- patch target (index 2)
#[cfg(target_os = "windows")]
fn windowed_exe(original: &std::path::Path) -> Result<std::path::PathBuf, String> {
    // Build `<stem>_windowed.exe` next to the original.
    let stem = original
        .file_stem()
        .ok_or("game_exe has no file stem")?
        .to_string_lossy();
    let patched = original
        .with_file_name(format!("{stem}_windowed.exe"));

    const PATCH_IDX: usize = 2;
    const BYTE_JZ:   u8    = 0x74;
    const BYTE_JMP:  u8    = 0xEB;

    let mut data = std::fs::read(original)
        .map_err(|e| format!("Failed to read game EXE: {e}"))?;

    // Scan for the 7-byte guard pattern.
    let found = data.windows(7).enumerate().find(|(_, w)| {
        w[0] == 0x85 && w[1] == 0xC0
            && (w[PATCH_IDX] == BYTE_JZ || w[PATCH_IDX] == BYTE_JMP)
            && w[3] == 0x15 && w[4] == 0x6A && w[5] == 0x01 && w[6] == 0x50
    });

    if let Some((offset, _)) = found {
        data[offset + PATCH_IDX] = BYTE_JMP;
    }
    // If pattern not found the copy is still created — it just won't be patched,
    // which is fine (launch will still work, just no multi-instance).

    // Use create_new (O_CREAT | O_EXCL) so only one process wins the race.
    // If another launcher got here first, AlreadyExists means the copy is ready
    // and we can use it as-is.
    use std::io::Write as _;
    match std::fs::OpenOptions::new().write(true).create_new(true).open(&patched) {
        Ok(mut f) => f.write_all(&data)
            .map_err(|e| format!("Failed to write windowed EXE copy: {e}"))?,
        Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => { /* another process created it */ }
        Err(e) => return Err(format!("Failed to create windowed EXE copy: {e}")),
    }

    Ok(patched)
}

#[cfg(target_os = "windows")]
pub fn launch_game(
    game_exe: &std::path::Path,
    windowed: bool,
    pcgi_path: &std::path::Path,
) -> Result<(), String> {
    let game_dir = game_exe.parent().ok_or("game_exe has no parent directory")?;
    let dest_ddraw = game_dir.join("ddraw.dll");

    let exe_to_launch: std::path::PathBuf;

    if windowed {
        // Write the ddraw shim.  Sharing violation is non-fatal — another
        // instance already placed it.
        if let Err(e) = std::fs::write(&dest_ddraw, DDRAW_DLL_BYTES) {
            if e.raw_os_error() != Some(ERROR_SHARING_VIOLATION) {
                return Err(format!("Failed to write ddraw.dll to game dir: {e}"));
            }
        }

        // Use the patched copy so we never touch a running EXE.
        exe_to_launch = windowed_exe(game_exe)?;
    } else {
        // Full-screen mode: remove the ddraw shim and use the original EXE.
        if dest_ddraw.exists() {
            let _ = std::fs::remove_file(&dest_ddraw);
        }
        exe_to_launch = game_exe.to_path_buf();
    }

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
    _windowed: bool,
    _pcgi_path: &std::path::Path,
) -> Result<(), String> {
    Err("MPBT only runs on Windows".to_string())
}

