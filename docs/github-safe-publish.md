# GitHub Safe Publish Checklist (No Secret Leaks)

Use this checklist before every push.

## 1) Never commit these

- Tokens (`factoryToken`, API keys, PATs, AWS secrets).
- Local credential files:
  - `tools/factory.settings.json` (already ignored in `.gitignore`)
- Logs/dumps that include private URLs or sensitive headers.

## 2) Safe to commit

- Firmware code (`src/`, `boards/`, `sysbuild/`, etc.).
- Documentation (`docs/`).
- Placeholder example files only:
  - `tools/factory.settings.json.example`

## 3) Mandatory checks before push

```powershell
git status --short
git diff -- . ':(exclude)build*'
```

### Search for sensitive patterns

```powershell
git grep -n -E "(factoryToken|x-factory-token|AKIA|SECRET|PRIVATE KEY|BEGIN RSA|BEGIN EC)"
```

If any real secret appears (not placeholders), remove it before commit.

## 4) Selective commit (recommended)

Do not use `git add .` in a large repo.

```powershell
git add docs/ src/ tools/factory_flash.py tools/factory_provision_build_flash.py
git status --short
```

Verify you did not add:
- `tools/factory.settings.json`
- files under `build*`
- temporary files (`.log`, `.tmp`)

## 5) Recommended release flow

1. Use a dedicated branch (`feature/...` or `release/...`).
2. Keep commits small and focused.
3. Open a PR with a checked security checklist.
4. Manually review diffs for sensitive strings.

## 6) If a leak happens

1. Revoke the leaked secret/token immediately.
2. Remove it from git history (not only latest commit).
3. Rotate all related credentials.
4. Redeploy with new secrets.
