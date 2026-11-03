# Contributing

You can help Eglantine without writing code:

- Report bugs and request features on [GitHub issues](https://github.com/notsapinho/Eglantine/issues). Search first, and attach a log ([how to find it](docs/user/HowToAccessLogs.md)).
- Share reproducible test results: which game and version, which GPU, driver and OS, and what you saw.
- Improve the documentation.
- Talk with the developers on the [Discord](https://discord.gg/jCCZ58VExk).

## Code contributions

Eglantine is free software under the [GPLv3](LICENSE.txt) (or any later version). By contributing you agree to license your work under it.

### Branches

- `dev` is where work happens. Open pull requests against `dev`.
- `main` is the release branch: CI builds and publishes a release on every push to it. Changes reach `main` from `dev`.

### Making a change

1. Fork [notsapinho/Eglantine](https://github.com/notsapinho/Eglantine) and create a branch from `dev`.
2. Build it for your platform ([docs/Build.md](docs/Build.md)) and test the behaviour you changed on real games.
3. Keep the change focused, follow the style of the surrounding code, and explain the why in the commit message.
4. Add or update license headers on the files you touched: run `.ci/license-header.sh` (see [docs/Development.md](docs/Development.md)). The pre-commit hook that CMake installs checks whitespace.
5. Open a pull request against `dev`, say what changed and why, and list the games, GPU and OS you tested on. Link any related issue.

Discuss larger features or refactors in an issue or on the Discord before starting, so two people don't build the same thing.

Never include game data, keys or firmware in a contribution.
