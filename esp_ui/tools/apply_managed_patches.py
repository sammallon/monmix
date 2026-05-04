#!/usr/bin/env python3
# Re-apply local hot-fixes to gitignored managed_components/ on every configure.
# Invoked from the project CMakeLists.txt so a fresh `idf.py reconfigure`
# (which the IDF component manager uses to regenerate managed_components/)
# can't silently revert the patch.

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# CMD52 fallback for ESP-Hosted SDIO TOKEN_RDATA polling.
# Why: a state machine in the C6's SLC HOST sub-block wedges after
# ~10-15 s of sustained slave->host TX activity, but ONLY for CMD53
# byte-mode register reads. CMD52 single-byte reads use a different
# command class on the slave and survive the storm. The 4x CMD52 cost
# is negligible compared to the TX bursts the bus is already carrying.
# Forensic record: memory project_outstanding_wifi_investigation.md.

SDIO_DRV = (
    ROOT
    / "managed_components"
    / "espressif__esp_hosted"
    / "host"
    / "drivers"
    / "transport"
    / "sdio"
    / "sdio_drv.c"
)

DEFINE_OLD = "#define DO_COMBINED_REG_READ (1)\n"
DEFINE_NEW = "#define DO_COMBINED_REG_READ (1)\n\n#define H_TOKEN_RDATA_MODE  1\n"

CALL_OLD = (
    "\tret = g_h.funcs->_h_sdio_read_reg(sdio_handle, ESP_SLAVE_TOKEN_RDATA, (uint8_t *)&len,\n"
    "\t\tsizeof(len), is_lock_needed);\n"
)
CALL_NEW = (
    "#if H_TOKEN_RDATA_MODE == 1\n"
    "\tuint8_t b[4];\n"
    "\tfor (int i = 0; i < 4; i++) {\n"
    "\t\tret = g_h.funcs->_h_sdio_read_reg(sdio_handle,\n"
    "\t\t\t\tESP_SLAVE_TOKEN_RDATA + i, &b[i], 1, is_lock_needed);\n"
    "\t\tif (ret) break;\n"
    "\t}\n"
    "\tif (ret == 0) {\n"
    "\t\tmemcpy(&len, b, sizeof(len));\n"
    "\t}\n"
    "#else\n"
    "\tret = g_h.funcs->_h_sdio_read_reg(sdio_handle, ESP_SLAVE_TOKEN_RDATA, (uint8_t *)&len,\n"
    "\t\tsizeof(len), is_lock_needed);\n"
    "#endif\n"
)


def patch_sdio_drv() -> bool:
    if not SDIO_DRV.exists():
        # Cold checkout: managed_components/ not resolved yet. The
        # component manager will fetch it during this configure pass;
        # the next configure picks up the patch.
        return False
    src = SDIO_DRV.read_text(encoding="utf-8")
    if "H_TOKEN_RDATA_MODE" in src:
        return False
    if DEFINE_OLD not in src or CALL_OLD not in src:
        sys.stderr.write(
            f"apply_managed_patches: anchors not found in {SDIO_DRV}.\n"
            "ESP-Hosted version drift -- update tools/apply_managed_patches.py.\n"
        )
        sys.exit(2)
    src = src.replace(DEFINE_OLD, DEFINE_NEW, 1)
    src = src.replace(CALL_OLD, CALL_NEW, 1)
    SDIO_DRV.write_text(src, encoding="utf-8")
    return True


if __name__ == "__main__":
    if patch_sdio_drv():
        print(
            "apply_managed_patches: applied CMD52 TOKEN_RDATA workaround "
            "to esp_hosted/sdio_drv.c"
        )
