# Memory experiments (`lkl_mem`)

`lkl_mem` runs kernel memory allocation experiments on request, and
provides `/dev/lkl_mmap`, a buffer that user space can map.

```sh
sudo insmod src/memory/lkl_mem.ko mmap_pages=16
echo "cache 10000" | sudo tee /sys/kernel/lkl_mem/run    # name argument
cat /sys/kernel/lkl_mem/result
```

A write fails with `EIO` if an invariant was violated and with `EINVAL`
for bad input. `result` holds the report either way.

| Experiment | Argument | What it checks |
|---|---|---|
| `kmalloc` | largest size (≤ 1 MiB) | For sizes from 1 byte up: the size class `kmalloc_size_roundup()` picks (13 → 16, 100 → 128, 5000 → 8192), `ARCH_KMALLOC_MINALIGN` alignment, and natural alignment for power-of-two sizes |
| `cache` | objects (≤ 100,000) | A `kmem_cache` with 64-byte alignment and a constructor. Every object is aligned. Over two rounds, every allocation returns an object in its constructed state, which only holds if objects are restored before `kmem_cache_free()`. |
| `pages` | order (≤ 8) | Four `alloc_pages()` blocks; each starts on a pfn that is a multiple of its size (a buddy allocator property) |
| `vmalloc` | bytes (≤ 64 MiB) | `vmalloc()` memory is virtually contiguous; reports how many physically contiguous runs back it |

## `/dev/lkl_mmap`

A `vmalloc_user()` buffer of `mmap_pages` pages (1–1024, default 16).

- `read`, `write` and `lseek` are bounded by the buffer size. Writing at
  the end gives `ENOSPC`.
- `mmap` supports only `MAP_SHARED`. Private mappings and mappings that
  extend past the buffer are rejected with `EINVAL`.
- Pages are not mapped up front. A `.fault` handler maps each page on
  first touch (`vmalloc_to_page()`). `/sys/kernel/lkl_mem/mmap_faults`
  counts the faults, so the tests can check there is one fault per page
  touched, and none on a second touch.
- A mapping grown with `mremap()` past the buffer gets `SIGBUS` there.
- A mapping keeps the module loaded even after its file descriptor is
  closed. The module reference belongs to the open file, which the
  mapping holds.

## Detector demonstrations

These run only when the detector is built in. Otherwise they are
refused with `EOPNOTSUPP`.

| Experiment | Bug | Expected report |
|---|---|---|
| `leak N` | `kmalloc(64)` N times, the pointers dropped. Each object holds `LKLLEAK-nnnn`. | kmemleak `unreferenced object`, with the text in its hex dump and `lkl_mem_leak_one` in the backtrace (needs `CONFIG_DEBUG_KMEMLEAK`) |
| `kasan_oob 1` | writes `p[13]` of a `kmalloc(13)` | `BUG: KASAN: slab-out-of-bounds`, `Write of size 1` (needs `CONFIG_KASAN`) |

`tests/debug/test_mem_detectors.sh` triggers both and requires the
reports. On the debug kernel (`scripts/build-debug-kernel.sh`) it needs
the `kasan_multi_shot` boot parameter, because the M4 demonstration has
already used KASAN's single report.
