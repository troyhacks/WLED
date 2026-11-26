Import('env')
import os

# The ESP-IDF's DWC2 USB host driver, and the P4's cache.
#
# The driver syncs its USB buffers with an ALIGNED cache sync - for the
# frame list:
#
#   esp_cache_msync(frame_list, FRAME_LIST_LEN * sizeof(uint32_t), 0)
#
# The frame list is FRAME_LIST_LEN words - 16 bytes - and the P4's cache
# line is 64 bytes, so an aligned sync of it is IMPOSSIBLE.  The IDF's
# esp_cache_msync() refuses it and says so, on every frame list update:
#
#   E cache: esp_cache_msync(122): start address: 0x4ff5e600, or the size:
#   0x10 is(are) not aligned with cache line size (0x40)B
#
# The driver asserts the sync succeeded (assert() is compiled out in a
# release build), so it carries on with a frame list that was never synced.
#
# The IDF already has the flag for this: ESP_CACHE_MSYNC_FLAG_UNALIGNED.
# The C2M direction (the CPU to memory) allows it - the M2C direction does
# not, so that one is left alone.
#
# The patch is applied to the framework package in place and it is
# idempotent: the script runs on every build and does nothing when the
# patch is already there.
#
# NOTE (2026-11-25): this error turned out to be NOISE rather than the
# cause of the control surface's trouble - the F1's OUT pipe was healthy
# (out_sent climbing, no timeouts, no dropped packets) with the error
# still coming.  It is patched because the error is one line from the
# driver believing its own frame list, not because it was the fault.

PATCHES = [
    # The frame list: 16 bytes, always C2M.
    ("esp_err_t ret = esp_cache_msync(frame_list, "
     "FRAME_LIST_LEN * sizeof(uint32_t), 0);",
     "esp_err_t ret = esp_cache_msync(frame_list, "
     "FRAME_LIST_LEN * sizeof(uint32_t), ESP_CACHE_MSYNC_FLAG_UNALIGNED);"),
    # The transfer descriptor list, on its C2M leg (mem_to_cache false).
    ("esp_err_t ret = esp_cache_msync(buffer->xfer_desc_list, "
     "buffer->xfer_desc_list_len_bytes, mem_to_cache ? "
     "ESP_CACHE_MSYNC_FLAG_DIR_M2C : 0);",
     "esp_err_t ret = esp_cache_msync(buffer->xfer_desc_list, "
     "buffer->xfer_desc_list_len_bytes, mem_to_cache ? "
     "ESP_CACHE_MSYNC_FLAG_DIR_M2C : ESP_CACHE_MSYNC_FLAG_UNALIGNED);"),
]


def framework_dirs():
    """Every place the framework-espidf package might be."""
    out = []
    try:
        out.append(env.PioPlatform().get_package_dir("framework-espidf"))
    except Exception:
        pass
    core = os.environ.get("PLATFORMIO_CORE_DIR")
    if core:
        out.append(os.path.join(core, "packages", "framework-espidf"))
    # The pioarduino platform ships its own copy of the framework.
    try:
        for pkg in env.PioPlatform().get_installed_packages():
            if pkg.metadata.get("name") == "framework-espidf":
                out.append(pkg.path)
    except Exception:
        pass
    seen = []
    for d in out:
        if d and d not in seen and os.path.isdir(d):
            seen.append(d)
    return seen


def driver_files():
    """Every hcd_dwc.c the patch has to reach.

    Up to ESP-IDF 5.5 the DWC2 driver was part of the IDF itself
    (components/usb/hcd_dwc.c).  From ESP-IDF 6.0 the USB component moved
    out of the IDF and into the Component Registry, so the driver comes
    from the managed component espressif/usb and lands in the project's
    managed_components directory.
    """
    out = []
    for pkg in framework_dirs():
        out.append(os.path.join(pkg, "components", "usb", "hcd_dwc.c"))
    project_dir = env.subst("$PROJECT_DIR")
    out.append(
        os.path.join(
            project_dir, "managed_components", "espressif__usb", "src", "hcd_dwc.c"
        )
    )
    return out


files = driver_files()
if not files:
    print("[dwc2] the framework-espidf package was not found - not patching")
else:
    for path in files:
        if not os.path.isfile(path):
            print("[dwc2] %s is not there - not patching" % path)
            continue
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            src = f.read()
        n = 0
        for old, new in PATCHES:
            if new in src:
                continue
            if old in src:
                src = src.replace(old, new)
                n += 1
        if n:
            with open(path, "w", encoding="utf-8") as f:
                f.write(src)
            print("[dwc2] patched %d cache sync(s) in %s: a 16-byte sync "
                  "cannot be aligned on a 64-byte cache line" % (n, path))
        else:
            print("[dwc2] %s is already patched" % path)
