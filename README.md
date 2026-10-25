# Client Library

OpenPak's Client Library.

| | |
| --- | --- |
| Network | Nintendo 3DS, DS, Wii and Wii U (through the emulators below) |
| Language | C++/Qt |
| Used by | [Azahar](https://github.com/openpak/azahar), [melonDS](https://github.com/openpak/melonds), [Marina](https://github.com/openpak/marina), [Quay](https://github.com/openpak/quay) |
| Licence | GPL-2.0-or-later |

## Packages

| Package | What |
| --- | --- |
| `client/` | `openpak::client`: HTTP client, account file, friend cache, network profile, `openpak::ZipStore` |
| `qt/` | `openpak::qt`: the OpenPak window, menu, sign-in, shared prompts, settings section and toasts, plus the `openpak::qt::Host` interface |
| `strings/` | The English string table and translations, generated into each toolkit's files |

## Use

Each consumer vendors the library as a git submodule and `add_subdirectory`s it. Qt hosts link
`openpak::qt`; a host without Qt links `openpak::client`. Details in [deploy](docs/deploy.md).

## Build

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release   # -DOPENPAK_BUILD_QT=OFF for the client only
cmake --build build
```

## Docs

- [Protocol](docs/protocol.md)
- [Evidence](docs/evidence.md)
- [Deploy](docs/deploy.md)
- [Notes](docs/notes.md)
- [Docs README](docs/README.md)

## Licence

GPL-2.0-or-later. See [LICENSE](LICENSE).
