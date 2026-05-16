# Cortex-M4 QEMU cross-check harness for plant_18433_cm4.S.
#
# Builds a bare-metal ELF that runs under qemu-system-arm with
# semihosting, exercising the asm against modq.h's C reference and
# reporting mismatches on stdout.
#
# Usage:
#   make            # build test_montymul.elf
#   make run        # run under QEMU (mps2-an386 board)
#   make clean

CROSS  ?= arm-none-eabi-
CC     := $(CROSS)gcc
OBJCOPY:= $(CROSS)objcopy
OBJDUMP:= $(CROSS)objdump
SIZE   := $(CROSS)size

CFLAGS  := -mcpu=cortex-m4 -mthumb -O2 -ffreestanding -nostdlib \
           -Wall -Wextra -Wno-unused-function -Wno-unused-variable \
           -fno-common -fdata-sections -ffunction-sections
ASFLAGS := -mcpu=cortex-m4 -mthumb -Wall
LDFLAGS := -mcpu=cortex-m4 -mthumb -nostdlib -nostartfiles \
           -Wl,--gc-sections -T qemu.ld

SRC_DIR := ../..
ASM_SRC := $(SRC_DIR)/plant_18433_cm4.S
TBL_SRC := $(SRC_DIR)/plant_18433_cm4_tables.c

.PHONY: all run run-ntt run-bench clean disasm
all: test_montymul.elf test_ntt.elf bench_ntt.elf

# Compile each piece into the m4/ subdir to keep artifacts contained.
plant_asm.o: $(ASM_SRC)
	$(CC) $(ASFLAGS) -c $< -o $@

plant_tables.o: $(TBL_SRC)
	$(CC) $(CFLAGS) -c $< -o $@

startup.o: startup.S
	$(CC) $(ASFLAGS) -c $< -o $@

test_montymul.o: test_montymul.c semihost.h
	$(CC) $(CFLAGS) -c test_montymul.c -o $@

test_montymul.elf: startup.o test_montymul.o plant_asm.o plant_tables.o qemu.ld
	$(CC) $(LDFLAGS) startup.o test_montymul.o plant_asm.o plant_tables.o -o $@
	@$(SIZE) $@

test_ntt.o: test_ntt.c semihost.h
	$(CC) $(CFLAGS) -c test_ntt.c -o $@

test_ntt.elf: startup.o test_ntt.o plant_asm.o plant_tables.o qemu.ld
	$(CC) $(LDFLAGS) startup.o test_ntt.o plant_asm.o plant_tables.o -o $@
	@$(SIZE) $@

bench_ntt.o: bench_ntt.c semihost.h
	$(CC) $(CFLAGS) -c bench_ntt.c -o $@

bench_c_refs.o: bench_c_refs.c
	$(CC) $(CFLAGS) -c bench_c_refs.c -o $@

bench_ntt.elf: startup.o bench_ntt.o bench_c_refs.o plant_asm.o plant_tables.o qemu.ld
	$(CC) $(LDFLAGS) startup.o bench_ntt.o bench_c_refs.o plant_asm.o plant_tables.o -o $@
	@$(SIZE) $@

QEMU = qemu-system-arm -M mps2-an386 -cpu cortex-m4 \
       -semihosting-config enable=on,target=native \
       -nographic -monitor none -serial none

# Force virtual time = instruction count (so SysTick advances).
# Only needed for run-bench; slows QEMU down for the correctness tests.
ICOUNT = -icount shift=0,align=off,sleep=off

run: test_montymul.elf
	$(QEMU) -kernel test_montymul.elf

run-ntt: test_ntt.elf
	$(QEMU) -kernel test_ntt.elf

run-bench: bench_ntt.elf
	$(QEMU) $(ICOUNT) -kernel bench_ntt.elf

disasm: test_montymul.elf
	$(OBJDUMP) -d $< | less

clean:
	rm -f *.o *.elf
