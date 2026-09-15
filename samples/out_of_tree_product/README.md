# Out-of-tree product sample

Product firmware that uses AkiraOS as a Zephyr module instead of forking it.
It is a stand-in for a product repository and references no AkiraOS source
file by path:

- `prj.conf` sets `CONFIG_AKIRA_OS=y`, which every `CONFIG_AKIRA_*` option
  depends on. The rest is copied from the AkiraOS reference firmware
  configuration, because several AkiraOS options still default to `y` without
  declaring the Zephyr features they need. Trim it once AkiraOS configuration
  profiles exist.
- `CMakeLists.txt` sets `AKIRA_SNIPPETS` to `akira-board`, which applies the
  AkiraOS flash layout, storage nodes and tuning for the selected board.
  Snippets passed with `west build -S` are applied after it.
- `CMakeLists.txt` links the `akira_os` interface target for include paths.
- `src/main.c` does product setup, then calls `akira_start()`.

Build it from the workspace:

```bash
west build -b native_sim AkiraOS/samples/out_of_tree_product
```

In a real product repository, `west.yml` imports `akira-os`; see
`zephyr/module.yml` for the manifest snippet.
