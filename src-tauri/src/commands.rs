use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Manager};

// ── DTOs ────────────────────────────────────────────────────────────────────

#[derive(Debug, Serialize, Deserialize)]
pub struct LoginResponse {
    pub username: String,
    pub email: String,
}

// ── Commands ─────────────────────────────────────────────────────────────────

/// Verify credentials against mpbt-web and return the stored email.
/// Called by the UI before launch to give fast feedback on bad credentials.
#[tauri::command]
pub async fn login(
    username: String,
    password: String,
    api_url: String,
) -> Result<LoginResponse, String> {
    do_login(&api_url, &username, &password).await
}

/// Called on launcher startup: remove any previously-placed ddraw.dll shim
/// from the game directory so a direct run of MPBTWIN.EXE always uses the
/// system DirectDraw (full-screen mode).
#[tauri::command]
pub fn cleanup_ddraw(game_exe: String) {
    let exe_path = std::path::Path::new(&game_exe);
    if let Some(game_dir) = exe_path.parent() {
        let shim = game_dir.join("ddraw.dll");
        if shim.exists() {
            let _ = std::fs::remove_file(shim);
        }
    }
}

/// Authenticate, write play.pcgi, launch MPBTWIN.EXE suspended,
/// inject ddraw.dll from the bundled resources, then resume.
#[tauri::command]
pub async fn launch_game(
    app: AppHandle,
    username: String,
    password: String,
    server: String,   // e.g. "127.0.0.1:2000"
    api_url: String,  // e.g. "http://localhost:3001"
    game_exe: String, // e.g. "C:\\MPBT\\MPBTWIN.EXE"
    windowed: bool,   // true → use ddraw shim for windowed mode
) -> Result<(), String> {
    // 1. Authenticate and retrieve email for play.pcgi
    let info = do_login(&api_url, &username, &password).await?;

    // 2. Parse port from server string ("host:port" or "host")
    let port: u16 = server
        .rsplit(':')
        .next()
        .and_then(|p| p.parse().ok())
        .unwrap_or(2000);

    // 3. Build play.pcgi content
    let pcgi = format!(
        "[launch]\nproduct = {port}\nserver = {server}\nServiceIdent = BATTLETECH\nAuthServ = g\n\n[identification]\nuser={user}\npassword={pass}\nemail={email}\n",
        port     = port,
        server   = server,
        user     = username,
        pass     = password,
        email    = info.email,
    );

    // 4. Write a per-launch play.pcgi into the game's directory.
    //    The filename includes both the sanitised username and the current
    //    process ID so that multiple simultaneous launches — even by the same
    //    user — each get their own file with no risk of overwriting each other.
    let exe_path = std::path::Path::new(&game_exe);
    let game_dir = exe_path
        .parent()
        .ok_or("invalid game_exe path")?;
    // Sanitise username for use as a filename component: keep only
    // alphanumeric characters and underscores, replace everything else.
    let safe_user: String = username
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() || c == '_' { c } else { '_' })
        .collect();
    let pcgi_path = game_dir.join(format!("play_{safe_user}_{}.pcgi", std::process::id()));
    std::fs::write(&pcgi_path, pcgi)
        .map_err(|e| format!("Failed to write {}: {e}", pcgi_path.display()))?;

    // 5. Deploy (or remove) shim and launch
    crate::game::launch_game(exe_path, windowed, &pcgi_path)?;

    // Close the launcher window.  Spawn a short-lived thread so the IPC
    // response can be flushed to the frontend before the window is torn down.
    let app2 = app.clone();
    std::thread::spawn(move || {
        std::thread::sleep(std::time::Duration::from_millis(300));
        if let Some(w) = app2.get_webview_window("main") {
            let _ = w.close();
        }
    });

    Ok(())
}

// ── Internal helpers ─────────────────────────────────────────────────────────

async fn do_login(
    api_url: &str,
    username: &str,
    password: &str,
) -> Result<LoginResponse, String> {
    let url = format!("{}/auth/login", api_url.trim_end_matches('/'));

    let body = serde_json::json!({
        "username": username,
        "password": password,
    });

    let client = reqwest::Client::new();
    let resp = client
        .post(&url)
        .json(&body)
        .send()
        .await
        .map_err(|e| format!("Could not reach the server: {e}"))?;

    if resp.status().is_success() {
        resp.json::<LoginResponse>()
            .await
            .map_err(|e| format!("Unexpected server response: {e}"))
    } else {
        let status = resp.status().as_u16();
        let text = resp.text().await.unwrap_or_default();
        // Try to extract NestJS message field
        let msg = serde_json::from_str::<serde_json::Value>(&text)
            .ok()
            .and_then(|v| {
                let m = v.get("message")?;
                if m.is_array() {
                    Some(
                        m.as_array()?
                            .iter()
                            .filter_map(|s| s.as_str())
                            .collect::<Vec<_>>()
                            .join(", "),
                    )
                } else {
                    m.as_str().map(str::to_owned)
                }
            })
            .unwrap_or_else(|| format!("HTTP {status}"));
        Err(msg)
    }
}
