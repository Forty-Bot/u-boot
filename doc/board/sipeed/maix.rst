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

    
    U-Boot 2020.01-00465-g1da52c6c9a (Feb 10 2020 - 20:26:50 -0500)
    
    DRAM:  8 MiB
    MMC:
    In:    serial@38000000
    Out:   serial@38000000
    Err:   serial@38000000
    =>

To boot a payload, first flash it to the board. This can be done by passing the
``-a <ADDRESS>`` option to kflash. After flashing your payload, load it by
running

.. code-block:: none

    => sf probe
    SF: Detected w25q128fw with page size 256 Bytes, erase size 4 KiB, total 16 MiB
    => sf read 80000000 <ADDRESS + 5> <SIZE>
    device 0 offset <ADDRESS + 5>, size <SIZE>
    SF: <SIZE> bytes @ <ADDRESS + 5> Read: OK
    => go 80000000
    ## Starting application at 0x80000000 ...

**NB:** kflash adds a 5-byte header to payloads (and a 32-byte trailer) to all
payloads it flashes. To load your payload properly, you will need to add 5 bytes
to the address that you gave to kflash.

The MMC (SD-card reader) does not work yet.

Over- and Under-clocking
------------------------

To change the clock speed of the K210, you will need to enable
``CONFIG_CLK_K210_SET_RATE`` and edit the board's device tree. To do this, add a
section to ``arch/riscv/arch/riscv/dts/k210-maix-bit.dts`` like the following:

.. code-block:: dts

    &sysclk {
 	assigned-clocks = <&sysclk K210_CLK_PLL0>;
 	assigned-clock-rates = <780000000>;
    };

There are three PLLs on the K210: PLL0 is the parent of most of the components,
including the CPU and RAM. PLL1 is the parent of the neural network coprocessor.
PLL2 is the parent of the sound processing devices. Note that child clocks of
PLL0 and PLL2 run at *half* the speed of the PLLs. For example, if PLL0 is
running at 800 MHz, then the CPU will run at 400 MHz.
