# Copilot Workflow Rules for mpbt-launcher

## Branch & PR Rules

- **Never commit directly to `master`.** Every change goes on a feature/fix/chore branch.
- **Never merge or close a PR.** Only the human merges. Open the PR and stop.
- After pushing a branch, open a PR and request review — then wait.

## Version Bumps

- Version is tracked in 5 files — bump all together:
  - `package.json`
  - `package-lock.json` (2 occurrences: top-level and inside `"packages": { "": { ... } }`)
  - `src-tauri/tauri.conf.json`
  - `src-tauri/Cargo.toml`
  - `src-tauri/Cargo.lock` (only the `[[package]] name = "mpbt-launcher"` entry)
- Always bump on a branch + PR, never directly on master.
- The release workflow triggers on every push to master and tags `v{version}`.
  Pushing the same version twice replaces the release silently — existing users
  on that version will NOT receive an auto-update notification.

## Versioning Strategy

- Direct-to-master commits that change launcher behavior require a subsequent
  version bump PR so auto-update triggers for existing users.

## Commit Style

- Use conventional commits: `fix(scope): message`, `chore: message`, `feat(scope): message`
