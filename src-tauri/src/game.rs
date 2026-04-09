/// game.rs — Game launch with optional DirectDraw windowed shim + multi-instance patch
///
/// When `windowed` is true:
///   • A sandbox runtime directory is created next to the original install.
///   • The game runtime files are mirrored there, preserving the executable's
///     original filename (`MPBTWIN.EXE` / `Mpbtwin.exe`), because the client
///     checks its command line and rejects renamed executables.
///   • The ddraw.dll shim is written only into the sandbox so the original
///     install stays untouched for normal full-screen launches.
///   • A patched sandbox copy of the EXE is created in place. Two patches are
///     applied: (a) the single-instance guard (JZ → JMP, one byte) so multiple
///     simultaneous instances are allowed, and (b) the self-integrity CRC
///     check bypass, because patching (a) changes the file's CRC which would
///     otherwise cause a "Fatal startup error" on launch.
///   • The sandboxed original-name EXE is what gets launched.
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

#[cfg(target_os = "windows")]
const SANDBOX_DIRNAME: &str = "__mpbt_launcher_windowed";

#[cfg(target_os = "windows")]
fn patch_windowed_exe(mut data: Vec<u8>) -> Vec<u8> {
    // The single-instance guard is a single `JZ` byte (`0x74`) changed to
    // `JMP` (`0xEB`). We locate it by scanning for the distinctive surrounding
    // bytes:
    //
    //   TEST EAX,EAX; Jcc +0x15; PUSH 1; PUSH EAX
    //   85 C0 [74|EB] 15 6A 01 50
    //           ^--- patch target (index 2)
    const PATCH_IDX: usize = 2;
    const BYTE_JZ: u8 = 0x74;
    const BYTE_JMP: u8 = 0xEB;

    if let Some((offset, _)) = data.windows(7).enumerate().find(|(_, w)| {
        w[0] == 0x85 && w[1] == 0xC0
            && (w[PATCH_IDX] == BYTE_JZ || w[PATCH_IDX] == BYTE_JMP)
            && w[3] == 0x15 && w[4] == 0x6A && w[5] == 0x01 && w[6] == 0x50
    }) {
        data[offset + PATCH_IDX] = BYTE_JMP;
    }

    // Patch 2: self-integrity CRC check bypass.
    // The game calls GetModuleFileNameA on itself, computes a CRC, and
    // compares it against a stored expected value. Since we changed a byte in
    // this copy the CRC will never match — bypassing the check is the only
    // practical option.
    //
    // The check ends with: SUB EAX,0xa; CMP EAX,0x1; SBB EAX,EAX; NEG EAX; RET
    // (returns 1 iff original return value was exactly 10 = CRC match)
    // We replace the whole sequence with: MOV EAX,1; NOP×5; RET
    // SBB EAX,EAX is encoded as 0x19 0xC0 (v1.06) or 0x1B 0xC0 (v1.23).
    const CRC_PATTERNS: [[u8; 11]; 2] = [
        [0x83, 0xE8, 0x0A, 0x83, 0xF8, 0x01, 0x19, 0xC0, 0xF7, 0xD8, 0xC3],
        [0x83, 0xE8, 0x0A, 0x83, 0xF8, 0x01, 0x1B, 0xC0, 0xF7, 0xD8, 0xC3],
    ];
    const CRC_PATCH: [u8; 11] = [
        0xB8, 0x01, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90, 0xC3,
    ];
    if let Some((off, _)) = data.windows(11).enumerate().find(|(_, w)| {
        CRC_PATTERNS.iter().any(|p| w == p)
    }) {
        data[off..off + 11].copy_from_slice(&CRC_PATCH);
    }

    data
}

#[cfg(target_os = "windows")]
fn sync_runtime_file(src: &std::path::Path, dst: &std::path::Path) -> Result<(), String> {
    let needs_copy = match (std::fs::metadata(src), std::fs::metadata(dst)) {
        (Ok(src_meta), Ok(dst_meta)) => {
            src_meta.len() != dst_meta.len()
                || src_meta.modified().ok() != dst_meta.modified().ok()
        }
        (Ok(_), Err(_)) => true,
        (Err(e), _) => return Err(format!("Failed to stat {}: {e}", src.display())),
    };

    if needs_copy {
        if let Some(parent) = dst.parent() {
            std::fs::create_dir_all(parent)
                .map_err(|e| format!("Failed to create {}: {e}", parent.display()))?;
        }
        std::fs::copy(src, dst)
            .map_err(|e| format!("Failed to copy {} to {}: {e}", src.display(), dst.display()))?;
    }

    Ok(())
}

#[cfg(target_os = "windows")]
fn sync_runtime_dir(src: &std::path::Path, dst: &std::path::Path) -> Result<(), String> {
    std::fs::create_dir_all(dst)
        .map_err(|e| format!("Failed to create {}: {e}", dst.display()))?;

    for entry in std::fs::read_dir(src)
        .map_err(|e| format!("Failed to read {}: {e}", src.display()))?
    {
        let entry = entry.map_err(|e| format!("Failed to read dir entry: {e}"))?;
        let src_path = entry.path();
        let dst_path = dst.join(entry.file_name());
        let file_type = entry
            .file_type()
            .map_err(|e| format!("Failed to stat {}: {e}", src_path.display()))?;

        if file_type.is_dir() {
            sync_runtime_dir(&src_path, &dst_path)?;
        } else if file_type.is_file() {
            sync_runtime_file(&src_path, &dst_path)?;
        }
    }

    Ok(())
}

#[cfg(target_os = "windows")]
fn prepare_windowed_sandbox(
    original_exe: &std::path::Path,
    pcgi_path: &std::path::Path,
) -> Result<(std::path::PathBuf, std::path::PathBuf), String> {
    let game_dir = original_exe
        .parent()
        .ok_or("game_exe has no parent directory")?;
    let sandbox_dir = game_dir.join(SANDBOX_DIRNAME);
    std::fs::create_dir_all(&sandbox_dir)
        .map_err(|e| format!("Failed to create sandbox dir {}: {e}", sandbox_dir.display()))?;

    for entry in std::fs::read_dir(game_dir)
        .map_err(|e| format!("Failed to read {}: {e}", game_dir.display()))?
    {
        let entry = entry.map_err(|e| format!("Failed to read dir entry: {e}"))?;
        let src_path = entry.path();
        let name = entry.file_name();
        let name_lossy = name.to_string_lossy();
        let name_lower = name_lossy.to_ascii_lowercase();
        let file_type = entry
            .file_type()
            .map_err(|e| format!("Failed to stat {}: {e}", src_path.display()))?;

        if file_type.is_dir() {
            if matches!(name_lower.as_str(), "music" | "sound" | "terrain" | "mechdata") {
                sync_runtime_dir(&src_path, &sandbox_dir.join(&name))?;
            }
            continue;
        }

        if !file_type.is_file() {
            continue;
        }

        if src_path == original_exe
            || name_lower == "ddraw.dll"
            || name_lower.ends_with("_windowed.exe")
            || (name_lower.starts_with("play") && name_lower.ends_with(".pcgi"))
            || name_lower.ends_with(".log")
        {
            continue;
        }

        sync_runtime_file(&src_path, &sandbox_dir.join(&name))?;
    }

    let patched_exe = sandbox_dir.join(
        original_exe
            .file_name()
            .ok_or("game_exe has no file name")?,
    );
    let patched_bytes = patch_windowed_exe(
        std::fs::read(original_exe)
            .map_err(|e| format!("Failed to read game EXE: {e}"))?,
    );
    std::fs::write(&patched_exe, patched_bytes)
        .map_err(|e| format!("Failed to write sandbox EXE {}: {e}", patched_exe.display()))?;

    let sandbox_pcgi = sandbox_dir.join("play.pcgi");
    std::fs::copy(pcgi_path, &sandbox_pcgi)
        .map_err(|e| format!("Failed to copy {} to {}: {e}", pcgi_path.display(), sandbox_pcgi.display()))?;

    let sandbox_ddraw = sandbox_dir.join("ddraw.dll");
    std::fs::write(&sandbox_ddraw, DDRAW_DLL_BYTES)
        .map_err(|e| format!("Failed to write {}: {e}", sandbox_ddraw.display()))?;

    Ok((patched_exe, sandbox_pcgi))
}

#[cfg(target_os = "windows")]
pub fn launch_game(
    game_exe: &std::path::Path,
    windowed: bool,
    pcgi_path: &std::path::Path,
) -> Result<(), String> {
    let game_dir = game_exe.parent().ok_or("game_exe has no parent directory")?;

    let exe_to_launch: std::path::PathBuf;
    let pcgi_to_launch: std::path::PathBuf;

    if windowed {
        // Keep the original install pristine and run a patched, sandboxed copy
        // that preserves the original executable filename.
        let dest_ddraw = game_dir.join("ddraw.dll");
        if dest_ddraw.exists() {
            let _ = std::fs::remove_file(&dest_ddraw);
        }
        (exe_to_launch, pcgi_to_launch) = prepare_windowed_sandbox(game_exe, pcgi_path)?;
    } else {
        // Full-screen mode: remove the ddraw shim and use the original EXE.
        let dest_ddraw = game_dir.join("ddraw.dll");
        if dest_ddraw.exists() {
            let _ = std::fs::remove_file(&dest_ddraw);
        }
        exe_to_launch = game_exe.to_path_buf();
        pcgi_to_launch = pcgi_path.to_path_buf();
    }

    std::process::Command::new(&exe_to_launch)
        .arg(&pcgi_to_launch)
        .current_dir(
            exe_to_launch
                .parent()
                .ok_or("launch target has no parent directory")?,
        )
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

