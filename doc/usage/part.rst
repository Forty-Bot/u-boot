.. SPDX-License-Identifier: GPL-2.0+
.. _partitions:

Partitions
==========

Many U-Boot commands allow specifying partitions like::

    some_command <interface> <devnum.hwpartnum:partnum>

or like::

    some_command <interface> <devnum.hwpartnum#partname

Where

  * ``interface`` is the device interface, like ``mmc`` or ``scsi``. For a full
    list of supported interfaces, consult the ``if_typename_str`` array in
    ``drivers/block/blk-uclass.c``
  * ``devnum`` is the device number. This defaults to 0.
  * ``hwpartnum`` is the hardware partition number. This defaults to 0 (the user
    partition on eMMC devices).
  * ``partname`` is the partition name on GPT devices. Partitions do not have
    names on MBR devices.
  * ``partnum`` is the partition number, starting from 1. The partition number 0
    is special, and specifies that the whole device is to be used as one
    "partition."

If neither ``partname`` nor ``partnum`` is specified and there is a partition
table, then partition 1 is used. If there is no partition table, then the whole
device is used as one "partition." If none of ``devnum``, ``hwpartnum``,
``partnum``, or ``partname`` is specified, then ``devnum`` defaults to the value
of the ``bootdevice`` environmental variable.
