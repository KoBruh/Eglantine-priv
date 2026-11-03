# Translation Scripts

The Qt translations in [`dist/languages`](../../dist/languages) come from Eden's Transifex project; this repository doesn't sync them itself (see its [README](../../dist/languages/README.md)).

- `qt-source.sh`: Regenerate the English source file for Qt (`dist/languages/en.ts`).
  * You must have `lupdate` in your PATH, e.g. `export PATH="/usr/lib64/qt6/bin:$PATH"` (requires `qttools`)
- `update-translations.sh`: Pull the translation files from Transifex and commit them. It needs a Transifex client configured with a `.tx/config`, which this repository doesn't have.
