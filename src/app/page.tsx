"use client";

import { useState, useEffect, FormEvent, InputHTMLAttributes, ChangeEventHandler } from "react";

// Defaults are injected at build time from NEXT_PUBLIC_* environment variables.
// In development these fall back to local values; for production builds the CI
// workflow supplies them from repository secrets.
const DEFAULT_SERVER  = process.env.NEXT_PUBLIC_DEFAULT_SERVER  ?? "127.0.0.1:2000";
const DEFAULT_API_URL = process.env.NEXT_PUBLIC_DEFAULT_API_URL ?? "http://localhost:3001";
const DEFAULT_GAME    = "C:\\MPBT\\MPBTWIN.EXE";
const STORAGE_KEY     = "mpbt_launcher_prefs";

interface Prefs {
  username: string;
  password: string;
  server: string;
  apiUrl: string;
  gameExe: string;
  savePassword: boolean;
  windowed: boolean;
}

interface NewsArticle {
  slug: string;
  title: string;
  summary: string;
}

interface UpdateInfo {
  version: string;
  install: () => Promise<void>;
}

function loadPrefs(): Prefs {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (raw) {
      const p = JSON.parse(raw) as Partial<Prefs>;
      return {
        username:     p.username     ?? "",
        password:     p.savePassword ? (p.password ?? "") : "",
        server:       p.server       ?? DEFAULT_SERVER,
        apiUrl:       p.apiUrl       ?? DEFAULT_API_URL,
        gameExe:      p.gameExe      ?? DEFAULT_GAME,
        savePassword: p.savePassword ?? false,
        windowed:     p.windowed     ?? false,
      };
    }
  } catch { /* ignore */ }
  return { username: "", password: "", server: DEFAULT_SERVER, apiUrl: DEFAULT_API_URL, gameExe: DEFAULT_GAME, savePassword: false, windowed: false };
}

function savePrefs(prefs: Prefs) {
  const toStore: Prefs = { ...prefs, password: prefs.savePassword ? prefs.password : "" };
  localStorage.setItem(STORAGE_KEY, JSON.stringify(toStore));
}

type Status = "idle" | "authenticating" | "launching" | "launched" | "error";

export default function LauncherPage() {
  const [username,     setUsername]     = useState("");
  const [password,     setPassword]     = useState("");
  const [server,       setServer]       = useState(DEFAULT_SERVER);
  const [apiUrl,       setApiUrl]       = useState(DEFAULT_API_URL);
  const [gameExe,      setGameExe]      = useState(DEFAULT_GAME);
  const [savePassword, setSavePassword] = useState(false);
  const [windowed,     setWindowed]     = useState(false);
  const [advanced,     setAdvanced]     = useState(false);
  const [hydrated,     setHydrated]     = useState(false);

  const [status, setStatus] = useState<Status>("idle");
  const [error,  setError]  = useState<string | null>(null);

  // Update banner state
  const [pendingUpdate, setPendingUpdate] = useState<UpdateInfo | null>(null);
  const [updateInstalling, setUpdateInstalling] = useState(false);

  // News state
  const [news, setNews] = useState<NewsArticle[]>([]);

  // Load persisted prefs on first render (client-only)
  useEffect(() => {
    const p = loadPrefs();
    setUsername(p.username);
    setPassword(p.password);
    setServer(p.server);
    setApiUrl(p.apiUrl);
    setGameExe(p.gameExe);
    setSavePassword(p.savePassword);
    setWindowed(p.windowed);
    setHydrated(true);

    // Remove any leftover ddraw.dll shim on every startup so a direct run
    // of MPBTWIN.EXE always uses full-screen mode.
    if (p.gameExe) {
      import("@tauri-apps/api/core").then(({ invoke }) =>
        invoke("cleanup_ddraw", { gameExe: p.gameExe }).catch(() => {})
      );
    }

    // Check for launcher updates — show an in-UI banner instead of window.confirm
    import("@tauri-apps/plugin-updater").then(async ({ check }) => {
      try {
        const update = await check();
        if (!update) return;
        setPendingUpdate({
          version: update.version,
          install: async () => {
            await update.downloadAndInstall();
            const { relaunch } = await import("@tauri-apps/plugin-process");
            await relaunch();
          },
        });
      } catch {
        // Silently ignore — update check is best-effort
      }
    }).catch(() => {});

    // Fetch latest 2 news articles from the web API
    const webBase = (process.env.NEXT_PUBLIC_DEFAULT_API_URL ?? "http://localhost:3001")
      .replace(/\/api\/?$/, "");
    fetch(`${webBase}/api/articles?limit=2`)
      .then((r) => (r.ok ? r.json() : Promise.reject()))
      .then((articles: NewsArticle[]) => setNews(articles))
      .catch(() => { /* best-effort */ });
  }, []);

  // Persist whenever any relevant value changes (after hydration)
  useEffect(() => {
    if (!hydrated) return;
    savePrefs({ username, password, server, apiUrl, gameExe, savePassword, windowed });
  }, [hydrated, username, password, server, apiUrl, gameExe, savePassword, windowed]);

  async function handleLaunch(e: FormEvent) {
    e.preventDefault();
    setError(null);
    setStatus("authenticating");

    try {
      // Dynamic import so the page works outside Tauri (plain browser) too
      const { invoke } = await import("@tauri-apps/api/core");

      setStatus("launching");
      await invoke("launch_game", {
        username,
        password,
        server,
        apiUrl,
        gameExe,
        windowed,
      });
      setStatus("launched");
    } catch (err) {
      setError(String(err));
      setStatus("error");
    }
  }

  const busy = status === "authenticating" || status === "launching";

  async function handleInstallUpdate() {
    if (!pendingUpdate) return;
    setUpdateInstalling(true);
    try {
      await pendingUpdate.install();
    } catch {
      setUpdateInstalling(false);
    }
  }

  async function openUrl(url: string) {
    try {
      const { open } = await import("@tauri-apps/plugin-shell");
      await open(url);
    } catch {
      window.open(url, "_blank");
    }
  }

  const webBase = (process.env.NEXT_PUBLIC_DEFAULT_API_URL ?? "http://localhost:3001")
    .replace(/\/api\/?$/, "");

  return (
    <main className="flex min-h-screen flex-col items-center justify-center bg-neutral-950 p-6">
      <div className="w-full max-w-sm flex flex-col gap-4">
        {/* Header */}
        <div className="text-center">
          <h1 className="text-4xl font-bold tracking-widest text-green-400">
            MPBT
          </h1>
          <p className="mt-1 text-xs text-neutral-500 uppercase tracking-widest">
            Solaris VII Revival
          </p>
        </div>

        {/* Update banner */}
        {pendingUpdate && (
          <div className="rounded-lg border border-yellow-700 bg-yellow-950/40 px-4 py-3 flex items-center justify-between gap-3">
            <div>
              <p className="text-sm font-semibold text-yellow-300">
                Update available — v{pendingUpdate.version}
              </p>
              <p className="text-xs text-yellow-600 mt-0.5">
                The launcher will restart automatically after installing.
              </p>
            </div>
            <button
              onClick={handleInstallUpdate}
              disabled={updateInstalling}
              className="shrink-0 rounded bg-yellow-600 hover:bg-yellow-500 disabled:opacity-50 px-3 py-1.5 text-xs font-bold text-black transition-colors"
            >
              {updateInstalling ? "Installing…" : "Install"}
            </button>
          </div>
        )}

        {status === "launched" ? (
          <div className="rounded-xl border border-green-800 bg-green-950/30 p-6 text-center text-green-400">
            <p className="text-lg font-semibold">Launching…</p>
            <p className="mt-1 text-sm text-neutral-400">
              The game is starting. This window can be closed.
            </p>
          </div>
        ) : (
          <form
            onSubmit={handleLaunch}
            className="flex flex-col gap-4 rounded-xl border border-neutral-800 bg-neutral-900 p-6"
          >
            <Field
              label="Username"
              id="username"
              value={username}
              onChange={(e) => setUsername(e.target.value)}
              autoComplete="username"
              required
              disabled={busy}
            />

            <Field
              label="Password"
              id="password"
              type="password"
              value={password}
              onChange={(e) => setPassword(e.target.value)}
              autoComplete="current-password"
              required
              disabled={busy}
            />

            <label className="flex items-center gap-2 text-xs text-neutral-500 cursor-pointer select-none">
              <input
                type="checkbox"
                checked={savePassword}
                onChange={(e) => setSavePassword(e.target.checked)}
                className="accent-green-500"
              />
              Remember password
            </label>

            <label className="flex items-center gap-2 text-xs text-neutral-500 cursor-pointer select-none">
              <input
                type="checkbox"
                checked={windowed}
                onChange={(e) => setWindowed(e.target.checked)}
                className="accent-green-500"
              />
              Launch in windowed mode
            </label>

            {/* Advanced settings toggle */}
            <button
              type="button"
              onClick={() => setAdvanced((v) => !v)}
              className="text-left text-xs text-neutral-500 hover:text-neutral-300 transition-colors"
            >
              {advanced ? "▾" : "▸"} Advanced
            </button>

            {advanced && (
              <div className="flex flex-col gap-4 border-t border-neutral-800 pt-4">
                <Field
                  label="Game Server"
                  id="server"
                  value={server}
                  onChange={(e) => setServer(e.target.value)}
                  placeholder="127.0.0.1:2000"
                  disabled={busy}
                />
                <Field
                  label="Auth API URL"
                  id="apiUrl"
                  value={apiUrl}
                  onChange={(e) => setApiUrl(e.target.value)}
                  placeholder="http://localhost:3001"
                  disabled={busy}
                />
                <Field
                  label="Game Executable"
                  id="gameExe"
                  value={gameExe}
                  onChange={(e) => setGameExe(e.target.value)}
                  placeholder="C:\MPBT\MPBTWIN.EXE"
                  disabled={busy}
                />
              </div>
            )}

            {error && (
              <p className="rounded-md border border-red-800 bg-red-950/40 px-3 py-2 text-sm text-red-400">
                {error}
              </p>
            )}

            <button
              type="submit"
              disabled={busy}
              className="mt-2 rounded-md bg-green-600 py-2.5 font-bold text-black transition-colors hover:bg-green-500 disabled:bg-neutral-700 disabled:text-neutral-500"
            >
              {status === "authenticating"
                ? "Authenticating…"
                : status === "launching"
                ? "Launching…"
                : "Launch Game"}
            </button>
          </form>
        )}

        {/* News section */}
        {news.length > 0 && (
          <div className="flex flex-col gap-2">
            <p className="text-xs font-semibold uppercase tracking-widest text-neutral-600">
              News
            </p>
            {news.map((article) => (
              <div
                key={article.slug}
                className="rounded-lg border border-neutral-800 bg-neutral-900 px-4 py-3"
              >
                <button
                  type="button"
                  onClick={() => openUrl(`${webBase}/articles/${article.slug}`)}
                  className="text-sm font-semibold text-green-400 hover:text-green-300 transition-colors text-left"
                >
                  {article.title}
                </button>
                <p className="mt-1 text-xs text-neutral-500 leading-relaxed">
                  {article.summary}
                </p>
              </div>
            ))}
          </div>
        )}
      </div>
    </main>
  );
}

interface FieldProps extends InputHTMLAttributes<HTMLInputElement> {
  label: string;
  id: string;
  onChange: ChangeEventHandler<HTMLInputElement>;
}

function Field({ label, id, ...props }: FieldProps) {
  return (
    <div className="flex flex-col gap-1">
      <label
        htmlFor={id}
        className="text-xs font-semibold uppercase tracking-widest text-neutral-400"
      >
        {label}
      </label>
      <input
        id={id}
        {...props}
        className="rounded-md border border-neutral-700 bg-neutral-800 px-3 py-2 text-neutral-100 placeholder-neutral-600 focus:outline-none focus:ring-2 focus:ring-green-500 disabled:opacity-50"
      />
    </div>
  );
}
