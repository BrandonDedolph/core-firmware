# Convenience wrapper around Meson + Ninja.
#
# This is the human entry point. Real build logic lives in meson.build.

BUILD_HW  := build-hw
BUILD_SIM := build-sim
BUILD_FUZZ := build-fuzz
FUZZ_SECS ?= 60
FUZZ_CC   ?= clang

.PHONY: help hw ipod sim fuzz fuzz-run clean realclean reconfigure-hw reconfigure-sim verify-hw

help:
	@echo "Targets:"
	@echo "  make hw           Build the firmware ELF + flat binary (ARM, iPod 5G/5.5G)"
	@echo "  make ipod         Build hw, then wrap as .ipod transport image"
	@echo "  make sim          Build the host simulator (SDL2)"
	@echo "  make fuzz         Build the libFuzzer harnesses (clang) into $(BUILD_FUZZ)/"
	@echo "  make fuzz-run     Run every harness for FUZZ_SECS (default 60) seconds"
	@echo "  make clean        Remove build artifacts"
	@echo "  make realclean    Remove build directories entirely"
	@echo
	@echo "Build directories:"
	@echo "  $(BUILD_HW)/      ARM cross-build (core.elf, core.bin, core.ipod)"
	@echo "  $(BUILD_SIM)/     host build"
	@echo "  $(BUILD_FUZZ)/    host build with clang + -Dfuzz=true (fuzz_<name>)"

hw: $(BUILD_HW)/build.ninja
	ninja -C $(BUILD_HW)

# `make ipod` produces $(BUILD_HW)/core.ipod from the flat binary that
# `make hw` already emits, with tools/pack_ipod.py (checksum + "ipvd" +
# image; it refuses an empty/truncated/blank core.bin). This used to be the
# desktop app's Go CLI (`go run ./cmd/core firmware pack`); the app has its
# own repository now (core-desktop-app) and the Python port is byte-identical.
# --force because remaking over a previous run's core.ipod is the normal case
# here, not an accident the packer's overwrite guard should block.
ipod: hw
	python3 ../tools/pack_ipod.py $(BUILD_HW)/core.bin --out $(BUILD_HW)/core.ipod --force

sim: $(BUILD_SIM)/build.ninja
	ninja -C $(BUILD_SIM)

# Static verification of the hw image that needs the linked ELF or the flat
# binary: crt0/linker layout asserts, the size budget, the host-findable
# version marker in core.bin, plus the header<->doc address consistency check
# (also run standalone in `meson test`). Depends on a fresh `make hw`. The MMIO
# trace tests run under `meson test`.
verify-hw: hw
	sh tests/scripts/check_hw_layout.sh $(BUILD_HW)/core.elf
	sh tests/scripts/check_size.sh $(BUILD_HW)/core.elf
	sh tests/scripts/check_version_marker.sh $(BUILD_HW)/core.bin
	python3 tests/scripts/check_hw_consistency.py
	python3 tests/scripts/check_name_hash_parity.py
	python3 tests/scripts/check_resume_parity.py
	python3 tests/scripts/check_index_loader.py
	python3 tests/scripts/check_build_index.py

# The fuzz harnesses (tests/fuzz, docs/testing-fuzz.md). A build directory of
# its own because it is a different COMPILER: libFuzzer is clang's, while the
# sim build is whatever cc is. Only the `fuzz` alias is built — the rest of
# the sim tree is configured here but never compiled with these flags.
fuzz: $(BUILD_FUZZ)/build.ninja
	ninja -C $(BUILD_FUZZ) -j4 fuzz

fuzz-run: fuzz
	sh tests/fuzz/run_fuzz.sh $(BUILD_FUZZ) $(FUZZ_SECS)

$(BUILD_FUZZ)/build.ninja:
	CC=$(FUZZ_CC) meson setup $(BUILD_FUZZ) -Dtarget=sim -Dfuzz=true -Db_lto=false

$(BUILD_HW)/build.ninja:
	meson setup $(BUILD_HW) --cross-file cross/arm-none-eabi.txt -Dtarget=hw

$(BUILD_SIM)/build.ninja:
	meson setup $(BUILD_SIM) -Dtarget=sim

reconfigure-hw:
	meson setup --reconfigure $(BUILD_HW) --cross-file cross/arm-none-eabi.txt -Dtarget=hw

reconfigure-sim:
	meson setup --reconfigure $(BUILD_SIM) -Dtarget=sim

clean:
	@if [ -d $(BUILD_HW)  ]; then ninja -C $(BUILD_HW)  -t clean; fi
	@if [ -d $(BUILD_SIM) ]; then ninja -C $(BUILD_SIM) -t clean; fi

realclean:
	rm -rf $(BUILD_HW) $(BUILD_SIM) $(BUILD_FUZZ)
