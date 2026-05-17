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

# Target: Cortex-M4 + Thumb-2. No float flags — HAWK is integer-only,
# arm-none-eabi-gcc defaults to soft-float without `-mfpu`, libgcc
# helpers emit only integer instructions, no FPU enable required.
#
# DEVIATION FROM §3.3 SPEC: the spec calls for `-mfpu=fpv4-sp-d16
# -mfloat-abi=hard` (pqm4 convention; some schemes like Falcon use
# doubles). HAWK doesn't, so we drop the FPU flags here. Documented
# in HANDOFF.md.
#
# Toolchain version: §3.3 says ARM GNU Toolchain 15.2; this harness was
# verified with 16.1.0 from homebrew. See HANDOFF.md for the deviation.
ARCH_FLAGS := -mcpu=cortex-m4 -mthumb

CFLAGS  := $(ARCH_FLAGS) -O2 -ffreestanding -nostdlib \
           -Wall -Wextra -Wno-unused-function -Wno-unused-variable \
           -fno-common -fdata-sections -ffunction-sections
ASFLAGS := $(ARCH_FLAGS) -Wall
LDFLAGS := $(ARCH_FLAGS) -nostdlib -nostartfiles \
           -Wl,--gc-sections -T qemu.ld

SRC_DIR := ../..
ASM_SRC := $(SRC_DIR)/plant_18433_cm4.S
TBL_SRC := $(SRC_DIR)/plant_18433_cm4_tables.c
BASIS_ASM_SRC := $(SRC_DIR)/basis_m2_mul_cm4.S

.PHONY: all run run-ntt run-bench run-smoke run-profile clean disasm
all: test_montymul.elf test_ntt.elf bench_ntt.elf smoke_hawk.elf profile_hawk.elf

# HAWK end-to-end smoke test: compiles every HAWK source for M4 with
# HAWK_PLANT_NTT=1 so the asm path is exercised through sign/verify.
SMOKE_HAWK_SRCS = \
  $(SRC_DIR)/hawk_kgen.c $(SRC_DIR)/hawk_sign.c $(SRC_DIR)/hawk_vrfy.c \
  $(SRC_DIR)/ng_fxp.c $(SRC_DIR)/ng_hawk.c $(SRC_DIR)/ng_mp31.c \
  $(SRC_DIR)/ng_ntru.c $(SRC_DIR)/ng_poly.c $(SRC_DIR)/ng_zint31.c \
  $(SRC_DIR)/sha3.c
SMOKE_HAWK_OBJS = $(addprefix smoke_, $(notdir $(SMOKE_HAWK_SRCS:.c=.o)))

MUPQ_COMMON = $(SRC_DIR)/../../../mupq/common
PQM4_COMMON = $(SRC_DIR)/../../../common
# pqm4's common/keccakf1600.S lacks `.type ..., %function` and
# `.thumb_func` directives at the entry points, so the linker reports
# "Unknown destination type (ARM/Thumb)" for direct calls from C.
# We awk-patch a local copy to add those directives (a TINY change worth
# upstreaming to pqm4 separately) so we get the optimized asm path
# rather than the C fallback in mupq/common/keccakf1600.c.
KECCAK_SRC_ORIG = $(PQM4_COMMON)/keccakf1600.S
KECCAK_SRC = keccakf1600_typed.S

SMOKE_CFLAGS = $(CFLAGS) -DHAWK_PLANT_NTT=1 -Istubs -I$(SRC_DIR) \
               -I$(SRC_DIR)/../../../mupq/common
SMOKE_LDFLAGS = $(LDFLAGS)

smoke_%.o: $(SRC_DIR)/%.c
	$(CC) $(SMOKE_CFLAGS) -DHAWK_MP_ASM_CORTEXM4=1 -c $< -o $@

libc_stubs.o: libc_stubs.c
	$(CC) $(CFLAGS) -c libc_stubs.c -o $@

smoke_hawk.o: smoke_hawk.c semihost.h
	$(CC) $(SMOKE_CFLAGS) -c smoke_hawk.c -o $@

keccakf1600_typed.S: $(KECCAK_SRC_ORIG)
	tr -d '\r' < $< | awk '/^\.global   Keccak/ {sym=$$2; print $$0 "\n.type     " sym ", %function\n.thumb_func"; next} 1' > $@

keccakf1600.o: $(KECCAK_SRC)
	$(CC) $(ASFLAGS) -c $< -o $@

smoke_hawk.elf: startup.o smoke_hawk.o libc_stubs.o $(SMOKE_HAWK_OBJS) \
                keccakf1600.o plant_asm.o plant_tables.o basis_m2_mul_asm.o qemu.ld
	$(CC) $(SMOKE_LDFLAGS) startup.o smoke_hawk.o libc_stubs.o $(SMOKE_HAWK_OBJS) \
	                       keccakf1600.o plant_asm.o plant_tables.o basis_m2_mul_asm.o -lgcc -o $@
	@$(SIZE) $@

# Compile each piece into the m4/ subdir to keep artifacts contained.
plant_asm.o: $(ASM_SRC)
	$(CC) $(ASFLAGS) -c $< -o $@

basis_m2_mul_asm.o: $(BASIS_ASM_SRC)
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

run-smoke: smoke_hawk.elf
	$(QEMU) -kernel smoke_hawk.elf

run-profile: profile_hawk.elf
	$(QEMU) $(ICOUNT) -kernel profile_hawk.elf

profile_hawk.o: profile_hawk.c semihost.h
	$(CC) $(SMOKE_CFLAGS) -c profile_hawk.c -o $@

PROF_WRAPS = -Wl,--wrap=mq18433_NTT_plant \
             -Wl,--wrap=mq18433_NTT_pair_plant \
             -Wl,--wrap=mq18433_iNTT_plant \
             -Wl,--wrap=mq18433_montymul_plant \
             -Wl,--wrap=KeccakF1600_StatePermute \
             -Wl,--wrap=KeccakF1600_StateXORBytes \
             -Wl,--wrap=KeccakF1600_StateExtractBytes \
             -Wl,--wrap=extract_lowbit \
             -Wl,--wrap=basis_m2_mul \
             -Wl,--wrap=sig_gauss \
             -Wl,--wrap=sig_gauss_alt \
             -Wl,--wrap=poly_symbreak \
             -Wl,--wrap=encode_sig \
             -Wl,--wrap=hawk_ntrugen_Hawk_regen_fg

# Profile build needs hawk_sign.c + hawk_vrfy.c compiled with HAWK_PROFILE
# so PROF_CALL/VPROF_CALL macros expand to cycle-counting wrappers.
prof_hawk_sign.o: $(SRC_DIR)/hawk_sign.c
	$(CC) $(SMOKE_CFLAGS) -DHAWK_PROFILE=1 -DHAWK_MP_ASM_CORTEXM4=1 -c $< -o $@
prof_hawk_vrfy.o: $(SRC_DIR)/hawk_vrfy.c
	$(CC) $(SMOKE_CFLAGS) -DHAWK_PROFILE=1 -DHAWK_MP_ASM_CORTEXM4=1 -c $< -o $@
prof_ng_hawk.o: $(SRC_DIR)/ng_hawk.c
	$(CC) $(SMOKE_CFLAGS) -DHAWK_PROFILE=1 -c $< -o $@

PROF_HAWK_OBJS = prof_hawk_sign.o prof_hawk_vrfy.o prof_ng_hawk.o \
                 smoke_hawk_kgen.o \
                 smoke_ng_fxp.o smoke_ng_mp31.o \
                 smoke_ng_ntru.o smoke_ng_poly.o smoke_ng_zint31.o \
                 smoke_sha3.o

profile_hawk.elf: startup.o profile_hawk.o libc_stubs.o $(PROF_HAWK_OBJS) \
                  keccakf1600.o plant_asm.o plant_tables.o basis_m2_mul_asm.o qemu.ld
	$(CC) $(SMOKE_LDFLAGS) $(PROF_WRAPS) startup.o profile_hawk.o libc_stubs.o $(PROF_HAWK_OBJS) \
	                       keccakf1600.o plant_asm.o plant_tables.o basis_m2_mul_asm.o -lgcc -o $@
	@$(SIZE) $@

# Pure-C baseline build: disables every asm gate so the resulting elf
# runs HAWK with no Plantard asm (HAWK_PLANT_NTT=0), no 31-bit mp_montymul
# inline asm (HAWK_MP_ASM_CORTEXM4 undefined), no basis_m2_mul asm leaves,
# and no sig_gauss inline asm. -U__ARM_FEATURE_DSP forces hawk_sign.c's
# basis_m2_mul body to fall through to its portable "holes" C path.
PURE_C_CFLAGS = $(CFLAGS) -DHAWK_PLANT_NTT=0 -U__ARM_FEATURE_DSP \
                -Istubs -I$(SRC_DIR) -I$(SRC_DIR)/../../../mupq/common

pc_%.o: $(SRC_DIR)/%.c
	$(CC) $(PURE_C_CFLAGS) -c $< -o $@

pc_profile_hawk.o: profile_hawk.c semihost.h
	$(CC) $(PURE_C_CFLAGS) -c profile_hawk.c -o $@

pc_hawk_sign.o: $(SRC_DIR)/hawk_sign.c
	$(CC) $(PURE_C_CFLAGS) -DHAWK_PROFILE=1 -c $< -o $@
pc_hawk_vrfy.o: $(SRC_DIR)/hawk_vrfy.c
	$(CC) $(PURE_C_CFLAGS) -DHAWK_PROFILE=1 -c $< -o $@
pc_ng_hawk.o:   $(SRC_DIR)/ng_hawk.c
	$(CC) $(PURE_C_CFLAGS) -DHAWK_PROFILE=1 -c $< -o $@

PC_HAWK_OBJS = pc_hawk_sign.o pc_hawk_vrfy.o pc_ng_hawk.o \
               pc_hawk_kgen.o pc_ng_fxp.o pc_ng_mp31.o \
               pc_ng_ntru.o pc_ng_poly.o pc_ng_zint31.o pc_sha3.o

pure_c_profile.elf: startup.o pc_profile_hawk.o libc_stubs.o $(PC_HAWK_OBJS) \
                    keccakf1600.o qemu.ld
	$(CC) $(SMOKE_LDFLAGS) $(PROF_WRAPS) startup.o pc_profile_hawk.o libc_stubs.o $(PC_HAWK_OBJS) \
	                       keccakf1600.o -lgcc -o $@
	@$(SIZE) $@

run-pure-c-profile: pure_c_profile.elf
	$(QEMU) $(ICOUNT) -kernel pure_c_profile.elf

disasm: test_montymul.elf
	$(OBJDUMP) -d $< | less

# Disassembly target for the sign object — useful when iterating on the
# sig_gauss[_alt] inner-loop inline asm in hawk_sign.c.
disasm-sign: smoke_hawk_sign.o
	$(OBJDUMP) -d $< -j .text > smoke_hawk_sign.dis
	@echo "wrote smoke_hawk_sign.dis"

clean:
	rm -f *.o *.elf
