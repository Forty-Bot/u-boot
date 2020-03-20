.. SPDX-License-Identifier: GPL-2.0+
.. Copyright (C) 2020 Sean Anderson <seanga2@gmail.com>

Maix Bit
========

Several of the Sipeed Maix series of boards cotain the Kendryte K210 processor,
a 64-bit RISC-V CPU. This processor contains several peripherals to accelerate
neural network processing and other "ai" tasks. This includes a "KPU" neural
network processor, an audio processor supporting beamforming reception, and a
digital video port supporting capture and output at VGA resolution. Other
peripherals include 8M of SRAM (accessible with and without caching); remappable
pins, including 40 GPIOs; AES, FFT, and SHA256 accelerators; a DMA controller;
and I2C, I2S, and SPI controllers. Maix peripherals vary, but include spi flash;
on-board usb-serial bridges; ports for cameras, displays, and sd cards; and
ESP32 chips. Currently, only the Sipeed Maix Bit V2.0 (bitm) is supported, but
the boards are fairly similar.

Documentation for Maix boards is available from
`Sipeed's website <http://dl.sipeed.com/MAIX/HDK/>`_.
Documentation for the Kendryte K210 is available from
`Kendryte's website <https://kendryte.com/downloads/>`_. However, hardware
details are rather lacking, so most technical reference has been taken from the
`standalone sdk <https://github.com/kendryte/kendryte-standalone-sdk>`_.

Build and boot steps
--------------------

To build u-boot, run

.. code-block:: none

    make sipeed_maix_bitm_defconfig
    make CROSS_COMPILE=<your cross compile prefix>

To flash u-boot to a maix bit, run

.. code-block:: none

    kflash -tp /dev/<your tty here> -B bit_mic u-boot-dtb.bin

Boot output should look like the following:

.. code-block:: none

    U-Boot 2020.04-rc2-00087-g2221cc09c1-dirty (Feb 28 2020 - 13:53:09 -0500)

    DRAM:  8 MiB
    MMC:   spi@53000000:slot@0: 0
    In:    serial@38000000
    Out:   serial@38000000
    Err:   serial@38000000
    Hit any key to stop autoboot:  0
    SF: Detected w25q128fw with page size 256 Bytes, erase size 4 KiB, total 16 MiB
    Reading 5242880 byte(s) at offset 0x00000000
    ## Starting application at 0x80000000 ...

Running Programs
^^^^^^^^^^^^^^^^

Binaries
""""""""

To run a bare binary, use the ``go`` command:

.. code-block:: none

    => loady
    ## Ready for binary (ymodem) download to 0x80000000 at 115200 bps...
    C
    *** file: ./examples/standalone/hello_world.bin
    $ sz -vv ./examples/standalone/hello_world.bin
    Sending: hello_world.bin
    Bytes Sent:   4864   BPS:649
    Sending:
    Ymodem sectors/kbytes sent:   0/ 0k
    Transfer complete

    *** exit status: 0 ***
    (CAN) packets, 5 retries
    ## Total Size      = 0x000012f8 = 4856 Bytes
    => go 80000000
    ## Starting application at 0x80000000 ...
    Example expects ABI version 9
    Actual U-Boot ABI version 9
    Hello World
    argc = 1
    argv[0] = "80000000"
    argv[1] = "<NULL>"
    Hit any key to exit ...

Legacy Images
"""""""""""""

To run legacy images, use the ``bootm`` command:

.. code-block:: none

    $ tools/mkimage -A riscv -O u-boot -T standalone -C none -a 80000000 -e 80000000 -d examples/standalone/hello_world.bin hello_world.img
    Image Name:
    Created:      Thu Mar  5 12:04:10 2020
    Image Type:   RISC-V U-Boot Standalone Program (uncompressed)
    Data Size:    4856 Bytes = 4.74 KiB = 0.00 MiB
    Load Address: 80000000
    Entry Point:  80000000

    $ picocom -b 115200 /dev/ttyUSB0
    => loady
    ## Ready for binary (ymodem) download to 0x80000000 at 115200 bps...
    C
    *** file: hello_world.img
    $ sz -vv hello_world.img
    Sending: hello_world.img
    Bytes Sent:   4992   BPS:665
    Sending:
    Ymodem sectors/kbytes sent:   0/ 0k
    Transfer complete

    *** exit status: 0 ***
    CAN) packets, 3 retries
    ## Total Size      = 0x00001338 = 4920 Bytes
    => bootm
    ## Booting kernel from Legacy Image at 80000000 ...
       Image Name:
       Image Type:   RISC-V U-Boot Standalone Program (uncompressed)
       Data Size:    4856 Bytes = 4.7 KiB
       Load Address: 80000000
       Entry Point:  80000000
       Verifying Checksum ... OK
       Loading Standalone Program
    Example expects ABI version 9
    Actual U-Boot ABI version 9
    Hello World
    argc = 0
    argv[0] = "<NULL>"
    Hit any key to exit ...

Flashing Images
---------------

To flash a kernel, transfer it over serial, then write it to the kernel
partition.

.. code-block:: none

    => loady 80000000 1500000
    ## Switch baudrate to 1500000 bps and press ENTER ...

    *** baud: 1500000

    *** baud: 1500000 ***
    ## Ready for binary (ymodem) download to 0x80000000 at 1500000 bps...
    C
    *** file: loader.bin
    $ sz -vv loader.bin
    Sending: loader.bin
    Bytes Sent:2478208   BPS:72937
    Sending:
    Ymodem sectors/kbytes sent:   0/ 0k
    Transfer complete

    *** exit status: 0 ***
    ## Total Size      = 0x0025d052 = 2478162 Bytes
    ## Switch baudrate to 115200 bps and press ESC ...

    *** baud: 115200

    *** baud: 115200 ***
    => sf probe
    SF: Detected w25q128fw with page size 256 Bytes, erase size 4 KiB, total 16 MiB
    => mtd write kernel 80000000 0 25d052
    Writing 2478162 byte(s) at offset 0x00000000

Partition Scheme
^^^^^^^^^^^^^^^^

There is no partition scheme specified by the manufacturer. The only requirement
imposed by the firmware is that offset 0 will be loaded and ran. The default
partition scheme is

========= ======== ======
Partition Offset   Size
========= ======== ======
u-boot    0x000000 496k
env       0x07C000 16k
kernel    0x080000 5M
data      0x580000 10.5M
========= ======== ======

**NB:** kflash adds a 5-byte header to payloads (and a 32-byte trailer) to all
payloads it flashes. If you use kflash to flash your payload, you will need to
account for this header when specifying what offset in spi flash to load from.

Pin Assignment
--------------

The K210 contains a Fully Programmable I/O Array (FPIOA), which can remap any of
its 256 input functions to any any of 48 output pins. The following table has
the default pin assignments for the BitM.

===== ========== =======
Pin   Function   Comment
===== ========== =======
IO_0  JTAG_TCLK
IO_1  JTAG_TDI
IO_2  JTAG_TMS
IO_3  JTAG_TDO
IO_4  UARTHS_RX
IO_5  UARTHS_TX
IO_6  GPIOHS_1
IO_7  GPIOHS_2
IO_8  GPIO_0
IO_9  GPIO_1
IO_10 GPIO_2
IO_11 GPIO_3
IO_12 GPIO_4     Green LED
IO_13 GPIO_5     Red LED
IO_14 GPIO_6     Blue LED
IO_15 GPIO_7
IO_16 GPIOHS_0   ISP
IO_17 GPIOHS_3
IO_18 I2S0_SCLK  MIC CLK
IO_19 I2S0_WS    MIC WS
IO_20 I2S0_IN_D0 MIC SD
IO_21 GPIOHS_4
IO_22 GPIOHS_5
IO_23 GPIOHS_6
IO_24 GPIOHS_7
IO_25 GPIOHS_8
IO_26 SPI1_D1    MMC MISO
IO_27 SPI1_SCLK  MMC CLK
IO_28 SPI1_D0    MMC MOSI
IO_29 GPIOHS_31  MMC CS
IO_30 GPIOHS_9
IO_31 GPIOHS_10
IO_32 GPIOHS_11
IO_33 GPIOHS_12
IO_34 GPIOHS_13
IO_35 GPIOHS_14
IO_36 GPIOHS_28  Panel CS
IO_37 GPIOHS_29  Panel RST
IO_38 GPIOHS_30  Panel DC
IO_39 SPI0_SCK   Panel WR
IO_40 SCCP_SDA
IO_41 SCCP_SCLK
IO_42 DVP_RST
IO_43 DVP_VSYNC
IO_44 DVP_PWDN
IO_45 DVP_HSYNC
IO_46 DVP_XCLK
IO_47 DVP_PCLK
===== ========== =======

Over- and Under-clocking
------------------------

To change the clock speed of the K210, you will need to enable
``CONFIG_CLK_K210_SET_RATE`` and edit the board's device tree. To do this, add a
section to ``arch/riscv/arch/riscv/dts/k210-maix-bit.dts`` like the following:

.. code-block::

    &sysclk {
	assigned-clocks = <&sysclk K210_CLK_PLL0>;
	assigned-clock-rates = <800000000>;
    };

There are three PLLs on the K210: PLL0 is the parent of most of the components,
including the CPU and RAM. PLL1 is the parent of the neural network coprocessor.
PLL2 is the parent of the sound processing devices. Note that child clocks of
PLL0 and PLL2 run at *half* the speed of the PLLs. For example, if PLL0 is
running at 800 MHz, then the CPU will run at 400 MHz. This is the example given
above. The CPU can be overclocked to around 600 MHz, and underclocked to 26 MHz.

It is possible to set PLL2's parent to PLL0. The plls are more accurate when
converting between similar frequencies. This makes it easier to get an accurate
frequency for I2S. As an example, consider sampling an I2S device at 44.1 kHz.
On this device, the I2S serial clock runs at 64 times the sample rate.
Therefore, we would like to run PLL2 at an even multiple of 2.8224 MHz. If
PLL2's parent is IN0, we could use a frequency of 390 MHz (the same as the CPU's
default speed).  Dividing by 138 yields a serial clock of about 2.8261 MHz. This
results in a sample rate of 44.158 kHz---around 50 Hz or .1% too fast. If,
instead, we set PLL2's parent to PLL1 running at 390 MHz, and request a rate of
2.8224 * 136 = 383.8464 MHz, the achieved rate is 383.90625 MHz. Dividing by 136
yields a serial clock of about 2.8228 MHz. This results in a sample rate of
44.107 kHz---just 7 Hz or .02% too fast. This configuration is shown in the
following example:

.. code-block::

    &sysclk {
	assigned-clocks = <&sysclk K210_CLK_PLL1>, <&sysclk K210_CLK_PLL2>;
	assigned-clock-parents = <0>, <&sysclk K210_CLK_PLL1>;
	assigned-clock-rates = <390000000>, <383846400>;
    };

There are a couple of quirks to the PLLs. First, there are more frequency ratios
just above and below 1.0, but there is a small gap around 1.0. To be explicit,
if the input frequency is 100 MHz, it would be impossible to have an output of
99 or 101 MHz. In addition, there is a maximum frequency for the internal VCO,
so higher input/output frequencies will be less accurate than lower ones.

Technical Details
-----------------

Boot Sequence
^^^^^^^^^^^^^

1. ``RESET`` pin is deasserted.
2. Both harts begin executing at ``0x00001000``.
3. Both harts jump to firmware at ``0x88000000``.
4. One hart is chosen as a boot hart.
5. Firmware reads value of pin ``IO_16`` (ISP).

   * If the pin is low, enter ISP mode. This mode allows loading data to ram,
     writing it to flash, and booting from specific addresses.
   * If the pin is high, continue boot.
6. Firmware reads the next stage from flash (SPI3) to address ``0x80000000``.

   * If byte 0 is 1, the next stage is decrypted using the built-in AES
     accelerator and the one-time programmable, 128-bit AES key.
   * Bytes 1 to 4 hold the length of the next stage.
   * The SHA-256 sum of the next stage is automatically calculated, and verified
     against the 32 bytes following the next stage.
7. The boot hart sends an IPI to the other hart telling it to jump to the next
   stage.
8. The boot hart jumps to ``0x80000000``.

Memory Map
^^^^^^^^^^

========== ========= ===========
Address    Size      Description
========== ========= ===========
0x00000000 0x1000    debug
0x00001000 0x1000    rom
0x02000000 0xC000    clint
0x0C000000 0x4000000 plic
0x38000000 0x1000    uarths
0x38001000 0x1000    gpiohs
0x40000000 0x400000  sram0 (non-cached)
0x40400000 0x200000  sram1 (non-cached)
0x40600000 0x200000  airam (non-cached)
0x40800000 0xC00000  kpu
0x42000000 0x400000  fft
0x50000000 0x1000    dmac
0x50200000 0x200000  apb0
0x50200000 0x80      gpio
0x50210000 0x100     uart0
0x50220000 0x100     uart1
0x50230000 0x100     uart2
0x50240000 0x100     spi slave
0x50250000 0x200     i2s0
0x50250200 0x200     apu
0x50260000 0x200     i2s1
0x50270000 0x200     i2s2
0x50280000 0x100     i2c0
0x50290000 0x100     i2c1
0x502A0000 0x100     i2c2
0x502B0000 0x100     fpioa
0x502C0000 0x100     sha256
0x502D0000 0x100     timer0
0x502E0000 0x100     timer1
0x502F0000 0x100     timer2
0x50400000 0x200000  apb1
0x50400000 0x100     wdt0
0x50410000 0x100     wdt1
0x50420000 0x100     otp control
0x50430000 0x100     dvp
0x50440000 0x100     sysctl
0x50450000 0x100     aes
0x50460000 0x100     rtc
0x52000000 0x4000000 apb2
0x52000000 0x100     spi0
0x53000000 0x100     spi1
0x54000000 0x200     spi3
0x80000000 0x400000  sram0 (cached)
0x80400000 0x200000  sram1 (cached)
0x80600000 0x200000  airam (cached)
0x88000000 0x20000   otp
0x88000000 0xC200    firmware
0x8801C000 0x1000    riscv priv spec 1.9 config
0x8801D000 0x2000    flattened device tree (contains only addresses and
                     interrupts)
0x8801F000 0x1000    credits
========== ========= ===========
