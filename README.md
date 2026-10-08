# gf180mcu-kianv-rv32ima-sv32

Hirosh Dabui's [KianV Sv32 Linux SoC](https://github.com/splinedrive/gf180mcu-kianv-rv32ima-sv32) as taped out on GF180MCU through wafer.space: the multi-cycle RV32IMA core with Sv32, Zicntr and Sstc, a UART, three SPI masters, a GPIO bit, CLINT, PLIC and an SDRAM controller, taken as a black box.

![maturity](https://img.shields.io/badge/maturity-planned-lightgrey) ![license](https://img.shields.io/badge/license-MIT%20OR%20Apache--2.0%20OR%20MulanPSL--2.0-blue) ![upstream](https://img.shields.io/badge/upstream-Apache--2.0-lightgrey)

Part of the [Tape-Out](https://github.com/Tape-Out) IP library, wired up by [`xirang`](https://github.com/Tape-Out/xirang). One submodule, not modified: `third_party/gf180mcu-kianv-rv32ima-sv32`. The core alone, with its ISA tests, is [`kianriscv`](https://github.com/Tape-Out/kianriscv).

## What this repository adds

The declaration in `ip.yaml` takes upstream's `chip_core`, the SoC without its GF180MCU pad ring: a clock, a reset and one 54-bit pad bus as `in`, `out` and `oe`. The macros are the ones upstream hardened with (`librelane/config.yaml`), except the caches.

| Pad | Signal | Pad | Signal |
|:--:|:--:|:--:|:--:|
| 0 | UART RX | 8 to 10 | flash CS, SCLK, MOSI |
| 1 | SPI0 MISO (SD card) | 11 to 13 | SPI1 CS, SCLK, MOSI (network) |
| 2 | flash MISO | 14 to 36 | SDRAM CLK, CKE, DQM[1:0], A[12:0], BA[1:0], CS, WE, RAS, CAS |
| 3 | SPI1 MISO (network) | 37 to 52 | SDRAM DQ[15:0] |
| 4 | UART TX | 53 | GPIO |
| 5 to 7 | SPI0 CS, SCLK, MOSI | | |

| Knob | Values | Default | What it sets |
|:--:|:--:|:--:|:--:|
| `systemClk` | 10 to 100 MHz | 50 MHz | SDRAM timing in cycles, the timer, the frequency software reads at `0x1000_0014` |
| `itlbEntries`, `dtlbEntries` | 8, 16, 32, 64 | 32 | TLB entries |

```console
$ ran test gf180mcu-kianv-rv32ima-sv32
```

## Memory map

| Address | What |
|:--:|:--:|
| `0x0200_0000` | CLINT |
| `0x0C00_0000` | PLIC; the UART is source 10 |
| `0x1000_0000`, `0x1000_0005` | UART data and line status |
| `0x1000_000C` | dividers: UART in the low half, SPI0 in the high half; 1 after reset |
| `0x1000_0010` | dividers: timer in the low half, SPI1 in the high half |
| `0x1000_0014`, `0x1000_0018` | clock in MHz as 8.8 fixed point, SDRAM bytes |
| `0x1000_0700` | GPIO direction, output, input |
| `0x1050_0000`, `0x1050_0100` | SPI0, SPI1 control and data |
| `0x1060_0000` | SDRAM controller settings |
| `0x1110_0000` | reboot (`0x7777`) |
| `0x2000_0000` | SPI flash, 16 MiB, read only; reset fetches from `0x2010_0000` |
| `0x8000_0000` | SDRAM, 32 MiB |

## Testing

`htest/tb.cpp` is a Verilator test bench with pin-level models of what sits outside the chip: an MT48LC16M16A2 SDRAM, an SPI flash that answers the `0x03` read, a UART at 115200, and with `+spiecho` an echo slave on each of the two SPI ports. The GPIO pad has a pull-up. It hangs on a module `tb` with the pad bus, so the same bench runs upstream's source (`htest/tb_rtl.v`) and a flattened tape-out file wrapped the same way. A script of `expect` and `send` lines decides the result.

The `hello` task boots a bare-metal program from the flash at `0x2010_0000`: it sets the UART divider from the frequency register, writes and reads back 1024 words spread over 16 MiB, checks that byte and halfword stores leave their neighbours alone, and reports over the UART. A second program, `htest/periph`, then goes through the peripherals: the GPIO driven both ways and released to the pull-up, four bytes exchanged with the echo slave on each SPI port across a re-asserted chip select, the timer interrupt, the UART's interrupt through the PLIC claimed as source 10, and a reboot through the reset register. Both run at every point of the matrix.

The `isa` task runs the standard riscv-tests (`p` environment; rv32ui, um, ua, mi and si) on the whole SoC: each program is placed in the SDRAM model, the flash holds two instructions that jump to it, and the bench watches `tohost`. All 79 that apply pass.

Not run, because the core does not implement them: `amocas_w` and `amocas_d` (Zacas, not part of A), `ma_data` (misaligned accesses in hardware are optional), `pmpaddr` (no PMP) and `breakpoint` (no debug triggers; `tselect` does not exist).

The `arch-test` task runs the unprivileged part of riscv-arch-test (ACT4) the same way: 71 self-checking programs for I, M, Zmmul, Zaamo, Zalrsc, Zicsr, Zifencei and Zicntr, all of which pass. The expected values come from the Sail model and are compiled into each program, which writes 1 or 3 to `tohost` and prints the mismatching register on the UART when it fails. `htest/arch-test/kianv-rv32ima/` is the configuration, derived from ACT4's `sail-RVI20U32` (no F, D, C or Zihpm; 32 MiB of RAM; output through the on-chip UART). `htest/arch-test/build.sh` compiles the programs; ACT4 draws fresh random operands on every build, so the pack is published once and pinned by digest in `htest/arch-test.pin`.

## Patches

The submodule is never edited. `htest/setup.sh` copies upstream's sources to `build/src` and applies what is under `patch/`; the black box is that tree.

| Patch | What it fixes |
|:--:|:--:|
| `msip.patch` | `mip.MSIP` did not follow the CLINT's `msip` down, so a machine software interrupt was taken again forever once raised. One line: MSIP joins the bits that are re-read every cycle. Reported as [upstream #3](https://github.com/splinedrive/gf180mcu-kianv-rv32ima-sv32/issues/3) |
| `mepc.patch` | `mepc` and `sepc` kept the two low bits software wrote; without C they read as zero |
| `jalr.patch` | `jalr` did not clear bit 0 of its target, so a target with that bit set trapped as misaligned. The next PC now always has bit 0 clear; every other target is even anyway, and `mret`/`sret` take another path |
| `lrsc.patch` | The reservation was a single bit, so after `lr.w` to one word and `lr.w` to another, `sc.w` to the first still succeeded. `lr.w` now also records the address and `sc.w` succeeds only on the same address |
| `lhu.patch` | A misaligned `lhu` neither loaded the right value nor trapped; it now raises a load-address-misaligned exception, as `lh` does |
| `shamt.patch` | A shift immediate with bit 5 set, reserved on RV32, executed; it now raises an illegal-instruction exception |
| `mafetch.patch` | A jump to a misaligned target wrote its link register, and the write-back state's prefetch of that target went out, so the handler's first instruction decoded the stale word. The link is no longer written and the prefetch is withheld |
| `mtval.patch` | On an illegal instruction `mtval` held only the opcode; it now holds the whole instruction |
| `mip.patch` | `mip` passed through two registers in a loop, so a CSR write to SSIP reached only one of them; the interrupt controller's copy is now combinational. Vectored `mtvec` also sent exceptions to base + 4 × cause; only interrupts are vectored now |
| `counters.patch` | `mcycle` and `minstret` did not exist. They are writable, the instruction that writes `minstret` is not counted, and `mcountinhibit` stops both |
| `tvm.patch` | With `mstatus.TVM` set, S-mode access to `satp` did not trap |
| `adbits.patch` | Translation ignored the A and D bits. A page with A clear, or a store to a page with D clear, now raises a page fault and software sets them (Svade) |
| `ptw.patch` | A superpage with nonzero low PPN bits was used with them masked, and a pointer at the last level was walked one level further; both now raise a page fault |

## Limits

Upstream's instruction and data caches are built on GF180MCU SRAM macros; here they are bypassed (`BYPASS_CACHES`), so every fetch and access goes to the SDRAM. The SDRAM size is a fixed macro in upstream's header, 32 MiB. The pad configuration outputs (`bidir_cs`, `bidir_sl`, `bidir_ie`, `bidir_pu`, `bidir_pd`) drive GF180MCU pad cells and have nowhere to go in another process. `spi_nor_flash.v` assigns one register both ways, which Verilator refuses by default; the bench passes `-Wno-BLKANDNBLK`. Upstream's own cocotb file is the unedited project template and tests nothing here.

## License

This repository: 任选其一 [MIT](LICENSE-MIT) · [Apache 2.0](LICENSE-APACHE) · [木兰宽松许可证 第2版](LICENSE-MULAN).

`third_party/gf180mcu-kianv-rv32ima-sv32` stays under **Apache-2.0**; the core files carry the author's ISC header.
