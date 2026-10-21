"""Post-build target: one image for a board's first USB flash.

    pio run -e t2can -t factory      -> .pio/build/t2can/firmware-factory.bin

An OTA update writes the app alone (firmware.bin). A blank board also needs the
bootloader, the partition table and boot_app0 (the otadata that boots app0),
each at its own offset. This merges all four into one file that goes to 0x0:

    esptool.py --chip esp32s3 write_flash 0x0 firmware-factory.bin

THE OFFSETS AND FLAGS ARE THE PLATFORM'S, not written here: FLASH_EXTRA_IMAGES
and ESP32_APP_OFFSET are the very list `pio run -t upload` hands esptool, and
the flash mode / frequency / size come from the same helpers its upload command
uses (platform-espressif32 builder/main.py). A partition table or board change
moves them in both places at once.

The release workflow (.github/workflows/release.yml) runs this for every env.
"""
Import("env")  # noqa: F821

env = env  # noqa: F821  (SCons injects it)


def _factory(target, source, env):
    board = env.BoardConfig()
    flash_size = board.get("upload.flash_size", "keep")
    if flash_size == "detect":          # a merged file has no board to ask
        flash_size = "keep"
    out = env.subst("$BUILD_DIR/${PROGNAME}-factory.bin")
    parts = []
    for offset, path in env.get("FLASH_EXTRA_IMAGES", []):
        parts += [env.subst(offset), env.subst(path)]
    parts += [env.subst("$ESP32_APP_OFFSET"), env.subst("$BUILD_DIR/${PROGNAME}.bin")]
    cmd = [
        '"$PYTHONEXE"', '"$OBJCOPY"',
        "--chip", board.get("build.mcu"),
        "merge_bin", "-o", '"%s"' % out,
        "--flash_mode", "${__get_board_flash_mode(__env__)}",
        "--flash_freq", "${__get_board_f_image(__env__)}",
        "--flash_size", flash_size,
    ] + ['"%s"' % p if not p.startswith("0x") else p for p in parts]
    print("factory: " + " ".join(parts))
    return env.Execute(" ".join(cmd))


env.AddCustomTarget(
    name="factory",
    dependencies="$BUILD_DIR/${PROGNAME}.bin",
    actions=_factory,
    title="Factory image",
    description="Bootloader + partitions + boot_app0 + app in one bin for 0x0",
)
