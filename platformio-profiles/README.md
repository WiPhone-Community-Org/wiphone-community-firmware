# PlatformIO firmware profiles

Use:

- `profiles/core.json`: core app and setting selection
- `profiles/full.json`: complete app and setting selection
- `profiles/books-only.json`: example custom app selection
- `tools/generate_extension_registry.py`: generated C++ registry metadata

Build the compatibility/full environment:

```sh
pio run -e wiphone-full
```

Build the standalone core:

```sh
pio run -e wiphone-core
```

Build an example custom selection:

```sh
WIPHONE_PROFILE=books-only pio run -e wiphone-full
```
