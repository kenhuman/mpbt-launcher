"use client";

import { useState, useEffect, useRef, FormEvent, InputHTMLAttributes, ChangeEventHandler } from "react";

// The single URL of the MPBT website — all config is derived from it at runtime.
const DEFAULT_WEB_URL = (process.env.NEXT_PUBLIC_WEB_URL ?? "http://localhost:3000").replace(/\/+$/, "");
const DEFAULT_GAME    = "C:\\MPBT\\MPBTWIN.EXE";
const STORAGE_KEY     = "mpbt_launcher_prefs";

interface ClientConfig {
  apiUrl: string;
  gameServer: string;
}

const DISPLAY_MODES = [
  { value: "fullscreen",        label: "Fullscreen (640×480 native)" },
  { value: "window-640x480",    label: "Windowed 640×480" },
  { value: "window-1024x768",   label: "Windowed 1024×768" },
  { value: "window-1280x960",   label: "Windowed 1280×960" },
  { value: "window-1920x1080",  label: "Windowed 1920×1080 (letterboxed)" },
  { value: "window-fullscreen", label: "Windowed Fullscreen" },
] as const;

type DisplayModeValue = typeof DISPLAY_MODES[number]["value"];

interface Prefs {
  username: string;
  password: string;
  webUrl: string;
  gameExe: string;
  savePassword: boolean;
  displayMode: DisplayModeValue;
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

function isValidDisplayMode(v: unknown): v is DisplayModeValue {
  return DISPLAY_MODES.some((m) => m.value === v);
}

function loadPrefs(): Prefs {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (raw) {
      const p = JSON.parse(raw) as Partial<Prefs> & { windowed?: boolean };
      // Migrate old boolean windowed pref
      const migratedMode: DisplayModeValue =
        isValidDisplayMode(p.displayMode) ? p.displayMode
        : p.windowed ? "window-640x480"
        : "fullscreen";
      return {
        username:     p.username     ?? "",
        password:     p.savePassword ? (p.password ?? "") : "",
        webUrl:       p.webUrl       ?? DEFAULT_WEB_URL,
        gameExe:      p.gameExe      ?? DEFAULT_GAME,
        savePassword: p.savePassword ?? false,
        displayMode:  migratedMode,
      };
    }
  } catch { /* ignore */ }
  return { username: "", password: "", webUrl: DEFAULT_WEB_URL, gameExe: DEFAULT_GAME, savePassword: false, displayMode: "fullscreen" };
}

function savePrefs(prefs: Prefs) {
  const toStore: Prefs = { ...prefs, password: prefs.savePassword ? prefs.password : "" };
  localStorage.setItem(STORAGE_KEY, JSON.stringify(toStore));
}

type Status = "idle" | "authenticating" | "launching" | "launched" | "error";

export default function LauncherPage() {
  const [username,     setUsername]     = useState("");
  const [password,     setPassword]     = useState("");
  const [webUrl,       setWebUrl]       = useState(DEFAULT_WEB_URL);
  const [gameExe,      setGameExe]      = useState(DEFAULT_GAME);
  const [savePassword, setSavePassword] = useState(false);
  const [displayMode,  setDisplayMode]  = useState<DisplayModeValue>("fullscreen");
  const [advanced,     setAdvanced]     = useState(false);
  const [hydrated,     setHydrated]     = useState(false);

  // Resolved at runtime from /api/client-config; not user-editable
  const [resolvedApiUrl,    setResolvedApiUrl]    = useState("");
  const [resolvedServer,    setResolvedServer]    = useState("");

  const [status, setStatus] = useState<Status>("idle");
  const [error,  setError]  = useState<string | null>(null);

  // Update banner state
  const [pendingUpdate, setPendingUpdate] = useState<UpdateInfo | null>(null);
  const [updateInstalling, setUpdateInstalling] = useState(false);

  // Synchronous guard — prevents a double-click from firing two launches before
  // the first setStatus("authenticating") re-render has a chance to disable the button.
  const launchingRef = useRef(false);

  // News state
  const [news, setNews] = useState<NewsArticle[]>([]);

  // Load persisted prefs on first render (client-only)
  useEffect(() => {
    const p = loadPrefs();
    setUsername(p.username);
    setPassword(p.password);
    setWebUrl(p.webUrl);
    setGameExe(p.gameExe);
    setSavePassword(p.savePassword);
    setDisplayMode(p.displayMode);
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

  }, []);

  // Re-fetch server config and news whenever the Web URL changes (after hydration).
  // Clears resolved values immediately so the launch button disables while in flight.
  useEffect(() => {
    if (!hydrated) return;
    setResolvedApiUrl("");
    setResolvedServer("");
    const base = webUrl.replace(/\/+$/, "");
    const controller = new AbortController();
    const { signal } = controller;

    fetch(`${base}/api/client-config`, { signal })
      .then((r) => (r.ok ? r.json() : Promise.reject()))
      .then((cfg: ClientConfig) => {
        setResolvedApiUrl(cfg.apiUrl);
        setResolvedServer(cfg.gameServer);
      })
      .catch(() => { /* best-effort; launch button stays disabled */ });

    fetch(`${base}/api/articles?limit=2`, { signal })
      .then((r) => (r.ok ? r.json() : Promise.reject()))
      .then((articles: NewsArticle[]) => setNews(articles))
      .catch(() => { /* best-effort */ });

    return () => controller.abort();
  }, [hydrated, webUrl]);

  // Persist whenever any relevant value changes (after hydration)
  useEffect(() => {
    if (!hydrated) return;
    savePrefs({ username, password, webUrl, gameExe, savePassword, displayMode });
  }, [hydrated, username, password, webUrl, gameExe, savePassword, displayMode]);

  async function handleLaunch(e: FormEvent) {
    e.preventDefault();
    if (launchingRef.current) return;
    launchingRef.current = true;
    setError(null);
    setStatus("authenticating");

    try {
      // Dynamic import so the page works outside Tauri (plain browser) too
      const { invoke } = await import("@tauri-apps/api/core");

      setStatus("launching");
      await invoke("launch_game", {
        username,
        password,
        server: resolvedServer,
        apiUrl: resolvedApiUrl,
        gameExe,
        displayMode,
      });
      setStatus("launched");
    } catch (err) {
      setError(String(err));
      setStatus("error");
      launchingRef.current = false;
    }
  }

  const busy = status === "authenticating" || status === "launching";
  const configReady = resolvedApiUrl !== "" && resolvedServer !== "";

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

  const webBase = webUrl.replace(/\/+$/, "");

  return (
    <main className="flex h-screen overflow-hidden bg-neutral-950">
      {/* ── Left column: login controls ── */}
      <div className="flex flex-col gap-4 p-6 w-[360px] shrink-0 justify-center overflow-y-auto">
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

            <div className="flex flex-col gap-1">
              <label
                htmlFor="displayMode"
                className="text-xs font-semibold uppercase tracking-widest text-neutral-400"
              >
                Display Mode
              </label>
              <select
                id="displayMode"
                value={displayMode}
                onChange={(e) => setDisplayMode(e.target.value as DisplayModeValue)}
                disabled={busy}
                className="rounded-md border border-neutral-700 bg-neutral-800 px-3 py-2 text-neutral-100 focus:outline-none focus:ring-2 focus:ring-green-500 disabled:opacity-50"
              >
                {DISPLAY_MODES.map((m) => (
                  <option key={m.value} value={m.value}>{m.label}</option>
                ))}
              </select>
            </div>

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
                  label="Web URL"
                  id="webUrl"
                  value={webUrl}
                  onChange={(e) => setWebUrl(e.target.value)}
                  placeholder="http://localhost:3000"
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
              disabled={busy || !configReady}
              className="mt-2 rounded-md bg-green-600 py-2.5 font-bold text-black transition-colors hover:bg-green-500 disabled:bg-neutral-700 disabled:text-neutral-500"
            >
              {status === "authenticating"
                ? "Authenticating…"
                : status === "launching"
                ? "Launching…"
                : !configReady
                ? "Connecting…"
                : "Launch Game"}
            </button>
          </form>
        )}

      </div>

      {/* ── Right column: news ── */}
      <div className="flex flex-col flex-1 p-6 border-l border-neutral-800 gap-3 justify-center overflow-hidden">
        <p className="text-xs font-semibold uppercase tracking-widest text-neutral-600">
          News
        </p>
        {news.length > 0 ? (
          news.map((article) => (
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
          ))
        ) : (
          <p className="text-xs text-neutral-700">No articles available.</p>
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
