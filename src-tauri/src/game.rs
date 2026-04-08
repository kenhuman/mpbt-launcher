/// game.rs — Game launch with optional DirectDraw windowed shim
///
/// When `windowed` is true the ddraw.dll shim bytes (embedded at compile time
/// from native/ddraw.dll) are written into the game directory.  Windows DLL
/// search order (application directory first) means the shim is loaded in
/// place of the system DirectDraw, keeping the game in a normal window.
/// When `windowed` is false any previously-placed shim is removed.
///
/// Embedding the bytes in the binary avoids Windows Defender quarantining a
/// standalone ddraw.dll sitting in the build tree.

/// DLL bytes baked in at compile time.  `native/build.bat` must have been run
/// before `cargo build` / `tauri dev`.
#[cfg(target_os = "windows")]
static DDRAW_DLL_BYTES: &[u8] = include_bytes!("../../native/ddraw.dll");

/// Windows ERROR_SHARING_VIOLATION — the file is open by another process.
#[cfg(target_os = "windows")]
const ERROR_SHARING_VIOLATION: i32 = 32;

#[cfg(target_os = "windows")]
pub fn launch_game(
    game_exe: &std::path::Path,
    windowed: bool,
    pcgi_path: &std::path::Path,
) -> Result<(), String> {
    let game_dir = game_exe.parent().ok_or("game_exe has no parent directory")?;
    let dest_ddraw = game_dir.join("ddraw.dll");

    if windowed {
        // Windowed mode: write the embedded shim bytes into the game dir.
        // If another instance already deployed the DLL and currently has it
        // open, Windows may report a sharing violation (os error 32); treat
        // that specific case as non-fatal and continue launching.
        if let Err(e) = std::fs::write(&dest_ddraw, DDRAW_DLL_BYTES) {
            if e.raw_os_error() != Some(ERROR_SHARING_VIOLATION) {
                return Err(format!("Failed to write ddraw.dll to game dir: {e}"));
            }
        }
    } else {
        // Full-screen mode: remove our shim if present.
        if dest_ddraw.exists() {
            let _ = std::fs::remove_file(&dest_ddraw);
        }
    }

    // Launch game with play.pcgi as argv[1] and game directory as cwd.
    std::process::Command::new(game_exe)
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

