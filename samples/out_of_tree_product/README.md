# Out-of-tree product sample

Product firmware that uses AkiraOS as a Zephyr module instead of forking it.
It is a stand-in for a product repository and references no AkiraOS source
file by path:

- `prj.conf` is one line, `CONFIG_AKIRA_OS=y`, plus any product options.
- `CMakeLists.txt` sets `AKIRA_SNIPPETS` to `akira-profile-minimal;akira-board`:
  the profile turns on the AkiraOS services this product needs (WASM runtime,
  app manager, storage, settings), and `akira-board` applies the flash layout
  and tuning for the selected board. Swap in `akira-profile-connected` for a
  networked product. Snippets passed with `west build -S` come after these.
- `CMakeLists.txt` links the `akira_os` interface target for include paths.
- `src/main.c` does product setup, then calls `akira_start()`.

Build it from the workspace:

```bash
west build -b native_sim AkiraOS/samples/out_of_tree_product
```

In a real product repository, `west.yml` imports `akira-os`; see
`zephyr/module.yml` for the manifest snippet.
