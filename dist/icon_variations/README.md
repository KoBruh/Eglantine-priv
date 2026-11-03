# Icon variations

These icons are licensed under GPLv3. [`tools/update-icons.sh`](../../tools/update-icons.sh) generates every icon (Windows, macOS, Qt, Android) from one variation; see the [tools README](../../tools/README.md).

- `base.svg`: The Eglantine icon, used by default (`VARIATION=base`).
- `base_bgcolor`: Its background colour, used for the Android adaptive and legacy launcher icons.

A new variation needs both files, `<name>.svg` and `<name>_bgcolor`; build it with `VARIATION=<name> tools/update-icons.sh`.

Try to keep the icons simple. And preferably automatically be able to be generated (to reduce maintanaince burden). Additionally keep the files small (use `svgo` and `optipng`) to not clutter the git.
