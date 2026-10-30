Spinel interface
################

The optional Spinel interface lets a host processor control the DECT NR+ stack
over a serial link, like the OpenThread radio co-processor (RCP) interface.
It reuses the Spinel protocol, its encoding and its HDLC-lite framing, so
existing Spinel tooling (for example pyspinel) can talk to an Opener device.

Control is layered. This version exposes the **driver level**: the host
schedules radio operations (TX, RX, RSSI1), reads the modem time and the
radio capabilities, and receives the driver events. The MAC, DLC and
convergence layers then run on the host. Property ranges are reserved for the
higher levels, which later versions can expose on the device.

The identifiers below are defined in :file:`include/zephyr/net/dectnrp_spinel.h`,
which host tools mirror.

Enabling the interface
**********************

The interface needs a DECT NR+ driver with the scheduled API
(``CONFIG_DECTNRP_DRIVER_SCHEDULED_API``) and networking:

.. code-block:: cfg

   CONFIG_NETWORKING=y
   CONFIG_DECTNRP_DRIVER_SCHEDULED_API=y
   CONFIG_DECTNRP_SPINEL=y

and a transport in the devicetree: an ``opener,spinel-spi`` node selects the
SPI transport (see `SPI`_), else a UART chosen as ``opener,spinel-uart``
selects the UART transport:

.. code-block:: devicetree

   / {
           chosen {
                   opener,spinel-uart = &uart0;
           };
   };

   &uart0 {
           current-speed = <1000000>;
   };

The Spinel layer registers the DECT NR+ L2 itself: an application that
enables it does not define its own L2. The interface starts at boot, at
application init level. The driver stays stopped until the host sets
``DRV_ENABLED``.

Main options:

.. list-table::
   :header-rows: 1

   * - Option
     - Default
     - Meaning
   * - ``CONFIG_DECTNRP_SPINEL_MAX_FRAME_SIZE``
     - 1300
     - Largest Spinel frame, before transport framing. Fits a
       ``DECTNRP_MTU`` payload plus headers.
   * - ``CONFIG_DECTNRP_SPINEL_OP_SLOTS``
     - 8
     - Driver operations the host can have in flight.
   * - ``CONFIG_DECTNRP_SPINEL_EVENT_BUFS``
     - 16
     - Driver events waiting to be sent to the host.
   * - ``CONFIG_DECTNRP_SPINEL_RX_BUFS``
     - 4
     - Frames from the host waiting to be processed.
   * - ``CONFIG_DECTNRP_SPINEL_TX_BUFS``
     - 4
     - Replies waiting to be sent to the host.
   * - ``CONFIG_DECTNRP_SPINEL_RESET_TIMEOUT_MS``
     - 1000
     - Time ``CMD_RESET`` waits for the in-flight operations.

The ``dectnrp-spinel`` sample of opener-samples is a ready-made firmware for
the nRF9151 DK and the nRF9131 EK (UART), and for the Codium NR+Gate (SPI).

Transport
*********

UART with HDLC-lite framing
===========================

Frames use Spinel HDLC-lite framing:

* frames are delimited by the flag byte ``0x7E``. The device sends one before
  and one after each frame, and accepts frames with or without the leading
  flag;
* ``0x7E``, ``0x7D``, ``0x11``, ``0x13`` and ``0xF8`` are sent as ``0x7D``
  followed by the byte XOR ``0x20``;
* the frame carries a 16-bit FCS (CRC-16/X-25: reflected polynomial
  ``0x8408``, initial value ``0xFFFF``, final XOR ``0xFFFF``), sent least
  significant byte first, before the closing flag.

Frames with a bad FCS, or larger than the maximum frame size, are dropped and
counted in ``DRV_STATS``. Both directions are interrupt driven.

Notes for hosts:

* The boot notification is sent whether or not a host listens. A host that
  connects later starts with ``CMD_RESET`` rather than waiting for it.
* The device holds ``CONFIG_DECTNRP_SPINEL_RX_BUFS`` frames from the host at
  once. Frames received while all are in use are dropped without a reply and
  counted as transport errors. Hosts keep at most that many requests waiting
  for a reply.

SPI
===

The device is the SPI slave (mode 0, MSB first) and uses the Spinel SPI
framing of OpenThread, without HDLC. Every transaction starts with a 5-byte
header in both directions, followed by the raw Spinel frame:

.. code-block:: none

   HDR (u8) | RECV_LEN (u16 LE) | DATA_LEN (u16 LE) | frame

* ``HDR`` holds the flags RST (``0x80``), CRC (``0x40``) and CCF (``0x20``)
  and the pattern ``0b10`` in bits 1..0. CRC is not supported: a header with
  the CRC flag, or without the pattern, is invalid.
* ``RECV_LEN`` is the largest frame the sender accepts in this transaction,
  ``DATA_LEN`` the length of the frame it sends, 0 for none.
* A frame is sent only if the transaction clocked all its ``5 + DATA_LEN``
  bytes. A received frame is accepted if ``DATA_LEN`` is at most the
  receiver's ``RECV_LEN`` and the transaction clocked it whole.
* The device sets RST in its headers from boot until the first transaction
  of at least 5 bytes.

Two lines from the device complete the bus:

* IRQ, active high: the device has a frame for the host. This is the
  opposite polarity of OpenThread's interrupt line.
* BUSY, active high, an extension to the OpenThread specification: the
  device is not ready for a transaction. The device goes busy at the end of
  each transaction and is ready again once it has armed the next one. It
  updates IRQ before it releases BUSY, so a host reads IRQ only while BUSY
  is inactive.

A host transaction:

#. Wait until BUSY is inactive.
#. Clock a transaction when IRQ is active or the host has a frame to send:
   the header and its frame, padded to ``5 + 128`` bytes or more.
#. If the device's ``DATA_LEN`` is longer than the bytes clocked, wait until
   BUSY is inactive and clock a second transaction of ``5 + DATA_LEN`` bytes
   to receive the whole frame.
#. If the device's ``RECV_LEN`` was shorter than the host's frame, the frame
   was not received: send it again in a later transaction.

The device advertises ``RECV_LEN`` = ``CONFIG_DECTNRP_SPINEL_MAX_FRAME_SIZE``,
or 0 while all its ``CONFIG_DECTNRP_SPINEL_RX_BUFS`` receive buffers are in
use: the host then sends its frame again.
A frame that no host clocks within ``CONFIG_DECTNRP_SPINEL_SPI_TX_TIMEOUT_MS``
(default 1000) is counted as a transport error, and the device moves on to
the next frame. The frame already armed in the SPI slave stays there until a
host clocks it: a host that connects late first receives that frame, often
the boot notification. Frames queued while no host clocks are dropped. A host
that sends ``CMD_NOOP`` right after ``CMD_RESET`` knows that the reset
notification of its reset is the last one before the ``NOOP`` reply, as the
device handles frames in order.

The ``opener,spinel-spi`` node is a child of the SPI slave controller:

.. code-block:: devicetree

   &spi2 {
           compatible = "nordic,nrf-spis";
           def-char = <0xff>;

           spinel@0 {
                   compatible = "opener,spinel-spi";
                   reg = <0>;
                   spi-max-frequency = <8000000>;
                   irq-gpios = <&gpio0 9 GPIO_ACTIVE_HIGH>;
                   busy-gpios = <&gpio0 26 GPIO_ACTIVE_HIGH>;
           };
   };

Set the controller's default character (``def-char`` on nRF) to ``0xff``: a
transaction clocked while the device is not armed then reads an invalid
header, which the host drops. The slave uses the controller's own chip
select pin, not a CS GPIO.

Frames
******

Spinel encoding
===============

Values use the Spinel data types, all little endian:

.. list-table::
   :header-rows: 1

   * - Type
     - Meaning
   * - ``b``
     - boolean, one byte
   * - ``C`` / ``c``
     - unsigned / signed 8-bit integer
   * - ``S``
     - unsigned 16-bit integer
   * - ``L`` / ``l``
     - unsigned / signed 32-bit integer
   * - ``X``
     - unsigned 64-bit integer
   * - ``i``
     - packed unsigned integer (EXI varint), used for commands, property
       keys and statuses
   * - ``U``
     - UTF-8 string, zero terminated
   * - ``d``
     - data, prefixed with its length as ``S``

Header and commands
===================

Each frame starts with the Spinel header byte: flag ``0b10`` in bits 7..6,
interface ID 0 in bits 5..4 and a transaction ID (TID) in bits 3..0. Host
requests use TID 1 to 15 and the device echoes it in the reply. Frames the
device sends on its own (events, reset notification) use TID 0.

The header is followed by the command (``i``), the property key (``i``) and
the property value.

.. list-table::
   :header-rows: 1

   * - Command
     - Reply
   * - ``CMD_NOOP`` (0)
     - ``LAST_STATUS`` = ``OK``
   * - ``CMD_RESET`` (1)
     - ``LAST_STATUS`` = ``RESET_SOFTWARE``, TID 0 (see `Reset`_)
   * - ``CMD_PROP_VALUE_GET`` (2)
     - ``CMD_PROP_VALUE_IS`` (6) with the value
   * - ``CMD_PROP_VALUE_SET`` (3)
     - ``CMD_PROP_VALUE_IS`` (6) with the new value
   * - ``CMD_PROP_VALUE_INSERT`` (4)
     - ``CMD_PROP_VALUE_INSERTED`` (7)

Errors are replied as ``CMD_PROP_VALUE_IS(PROP_LAST_STATUS, status)`` with
the request TID:

* malformed frame or value: ``PARSE_ERROR``;
* unknown property, or a property that does not support the command:
  ``PROP_NOT_FOUND``;
* unknown command: ``INVALID_COMMAND``;
* driver errors: ``-EALREADY`` is ``ALREADY``, ``-ENOMEM`` is ``NOMEM``,
  ``-EBUSY`` is ``BUSY``, ``-EINVAL`` is ``INVALID_ARGUMENT``. Other errnos
  are vendor statuses ``DECTNRP_SPINEL_STATUS_ERRNO_BASE`` (``0x3C80``) plus
  the positive errno, capped at ``0x3CFF``.

Identity
========

The standard core properties identify the device:

.. list-table::
   :header-rows: 1

   * - Property
     - Value
   * - ``PROP_PROTOCOL_VERSION``
     - 4.3 (``ii``)
   * - ``PROP_NCP_VERSION``
     - ``OPENER/<major>.<minor>.<patch>-<commit>[+]; <driver>; <build date>``
       (``+`` marks a dirty tree)
   * - ``PROP_INTERFACE_TYPE``
     - ``0x4E52`` ("NR"), DECT NR+
   * - ``PROP_VENDOR_ID``
     - 0
   * - ``PROP_CAPS``
     - the levels the firmware exposes: ``DECTNRP_SPINEL_CAP_DRV``
       (``0x3D00``)
   * - ``PROP_LAST_STATUS``
     - ``RESET_POWER_ON`` sent at boot, ``RESET_SOFTWARE`` after
       ``CMD_RESET``

The capability values reserved for the higher levels are
``DECTNRP_SPINEL_CAP_MAC`` (``0x3E00``), ``DECTNRP_SPINEL_CAP_DLC``
(``0x3F00``) and ``DECTNRP_SPINEL_CAP_CVG`` (``0x3F80``).

Property ranges
===============

DECT NR+ properties use the Spinel vendor range ``0x3C00``–``0x3FFF``.
``0x3C00``–``0x3C7F`` is left free because ESP-IDF uses it.

.. list-table::
   :header-rows: 1

   * - Range
     - Level
   * - ``0x3C80``–``0x3CFF``
     - Opener core (reserved)
   * - ``0x3D00``–``0x3DFF``
     - driver
   * - ``0x3E00``–``0x3EFF``
     - MAC (reserved)
   * - ``0x3F00``–``0x3F7F``
     - DLC (reserved)
   * - ``0x3F80``–``0x3FFF``
     - convergence layer and applications (reserved)

Driver-level properties
***********************

.. list-table::
   :header-rows: 1

   * - Property
     - Key
     - Access
     - Format
     - Meaning
   * - ``DRV_ENABLED``
     - ``0x3D00``
     - get, set
     - ``b``
     - driver started; setting it calls ``start()`` or ``stop()``
   * - ``DRV_CAPS``
     - ``0x3D01``
     - get
     - see below
     - radio capabilities
   * - ``DRV_TIME``
     - ``0x3D02``
     - get
     - ``X``
     - modem time, in modem ticks
   * - ``DRV_NETWORK_ID``
     - ``0x3D03``
     - get, set
     - ``L``
     - network ID
   * - ``DRV_FILTER_SHORT_NWID``
     - ``0x3D04``
     - get, set
     - ``bC``
     - short network ID filter: active, value
   * - ``DRV_FILTER_SHORT_RXADDR``
     - ``0x3D05``
     - get, set
     - ``bS``
     - short receiver address filter: active, value
   * - ``DRV_OP_QUEUE``
     - ``0x3D10``
     - insert
     - see `Operations`_
     - schedule a driver operation
   * - ``DRV_EVENT``
     - ``0x3D11``
     - sent by the device
     - see `Events`_
     - driver events
   * - ``DRV_STATS``
     - ``0x3D20``
     - get
     - ``LLLLLLL``
     - interface counters

Getting the network ID or a filter returns the value the host last set.

``DRV_CAPS`` (``LCCCCcCLCCCCLSCLLLLLLLL``) holds, in order:

#. ``L`` hardware capability flags;
#. ``C`` release, ``C`` maximum power class, ``C`` maximum NSS for RX,
   ``C`` maximum RX/TX diversity, ``c`` RX gain in dB, ``C`` maximum MCS,
   ``L`` soft buffer size, ``C`` maximum HARQ processes, ``C`` HARQ feedback
   delay, ``C`` maximum mu, ``C`` maximum beta (the radio device capabilities
   of ETSI TS 103 636-4);
#. ``L`` modem ticks per symbol;
#. ``S`` maximum Spinel frame size;
#. ``C`` number of operation slots;
#. the driver latencies, in modem ticks (``L`` each): scheduled operation
   transition, scheduled operation startup, RX startup, RX shutdown, RSSI1
   shutdown, TX startup, TX shutdown, stop to RF off.

``DRV_STATS`` holds seven ``L`` counters: operations accepted, operations
rejected, events sent, events dropped, frames received from the host, frames
rejected (FCS or size), transport errors.

Operations
==========

``CMD_PROP_VALUE_INSERT(DRV_OP_QUEUE, …)`` schedules one driver operation. The
value is a common prefix (``SCSCX``):

* ``S`` token, chosen by the host, unique among the operations in flight;
* ``C`` type: 0 TX, 1 RX, 2 RSSI1;
* ``S`` channel;
* ``C`` flags;
* ``X`` start time, in modem ticks, used only when the operation is
  scheduled.

followed by a suffix that depends on the type:

.. list-table::
   :header-rows: 1

   * - Type
     - Format
     - Fields
   * - TX
     - ``CSCCCCLCd``
     - physical header type (0: type 1, 5 bytes; 1: type 2, 10 bytes), TBS,
       mu, beta, MCS, slots, LBT period, LBT RSSI threshold, data (physical
       header followed by at most ``DECTNRP_MTU`` bytes of payload)
   * - RX
     - ``LC``
     - duration in modem ticks, expected RSSI
   * - RSSI1
     - ``L``
     - number of subslots to measure

Flags:

.. list-table::
   :header-rows: 1

   * - Bits
     - Flag
     - Allowed for
   * - 0
     - scheduled: start at the start time, else immediately
     - all
   * - 1
     - listen before talk
     - TX
   * - 2
     - HARQ
     - TX
   * - 3
     - beacon
     - TX, RX
   * - 5..4
     - reception mode: 0 single shot, 1 semi-continuous, 2 continuous
     - RX

A flag set on a type that does not allow it, or reception mode 3, is
rejected with ``INVALID_ARGUMENT``.

The device replies ``CMD_PROP_VALUE_INSERTED(DRV_OP_QUEUE, token)`` once the
driver accepted the operation. Otherwise it replies ``LAST_STATUS``:

* ``NOMEM``: all operation slots are in use, or no TX buffer is free;
* ``ALREADY``: the token is in flight;
* ``INVALID_ARGUMENT`` or ``PARSE_ERROR``: a bad field, or data shorter than
  the physical header or longer than the physical header plus
  ``DECTNRP_MTU``, or TX data that does not fit in one network buffer (drivers
  transmit the first buffer of a packet only: set
  ``CONFIG_NET_BUF_DATA_SIZE`` to at least ``DECTNRP_MTU`` plus 10 bytes);
* the status of the driver error when ``schedule()`` failed.

Events
======

The device sends driver events as ``CMD_PROP_VALUE_IS(DRV_EVENT, …)`` with
TID 0. The value is a common prefix (``SClS``):

* ``S`` token of the operation;
* ``C`` event code;
* ``l`` status, as reported by the driver (0 means OK). For ``MSG_ERROR`` it
  is an ``enum dectnrp_error``;
* ``S`` number of events dropped since the previous event sent, saturating at
  65535.

followed by a suffix that depends on the code:

.. list-table::
   :header-rows: 1

   * - Code
     - Format
     - Fields
   * - 1 ``MSG_RECEIVED``
     - ``CCXCcSdd``
     - PHY type, valid bits (bit 0 RSSI-2, bit 1 SNR), start time, RSSI-2,
       SNR, channel, physical header, payload
   * - 2 ``MSG_ERROR``
     - ``CCXCcSd``
     - PHY type, valid bits (bit 0 RSSI-2, bit 1 SNR, bit 2 physical header),
       start time, RSSI-2, SNR, channel, physical header (empty when not
       valid)
   * - 3 ``OP_RESULTS``
     - ``Sd``
     - channel, one RSSI-1 byte per subslot
   * - 4 ``OP_FINISHED``
     - (none)
     - the operation is over

RSSI-2 and SNR are forwarded as the driver encodes them (ETSI TS 103 636-2
clauses 8.3 and 8.4). The channel is the one of the operation that produced
the event.

Delivery guarantees
===================

After ``INSERTED``, the device sends exactly one ``OP_FINISHED`` for the
token, and it is the last event of the operation. Only a reset breaks this
rule (see `Reset`_). The token can be reused once its ``OP_FINISHED`` is
received.

The other events share a pool of ``CONFIG_DECTNRP_SPINEL_EVENT_BUFS`` buffers.
When it is exhausted, ``MSG_RECEIVED``, ``MSG_ERROR`` and ``OP_RESULTS`` are
dropped, counted in ``DRV_STATS``, and reported in the ``dropped`` field of the
next event sent. ``OP_FINISHED`` has its own buffer per operation slot and is
never dropped, so a host never waits forever for it.

Reset
=====

``CMD_RESET`` resets the interface without rebooting the device:

#. it stops the driver (``DRV_ENABLED`` becomes false), which ends the
   operations in flight;
#. it waits at most ``CONFIG_DECTNRP_SPINEL_RESET_TIMEOUT_MS`` for their
   ``OP_FINISHED`` events, which are sent, then frees all slots;
#. it deactivates the short network ID and short receiver address filters;
   the network ID is kept;
#. it sends ``LAST_STATUS`` = ``RESET_SOFTWARE`` with TID 0.

A reset, or a reboot (``RESET_POWER_ON``), invalidates every token in flight:
the host forgets them, and must not expect ``OP_FINISHED`` for them once the
reset notification is received.

Quick start on the nRF9151 DK
*****************************

#. Program the DECT NR+ PHY modem firmware on the DK.
#. Build and flash the ``dectnrp-spinel`` sample of opener-samples:

   .. code-block:: bash

      west build -p -b nrf9151dk/nrf9151/ns samples/dectnrp-spinel
      west flash

#. The Spinel UART is ``uart0``, the first virtual COM port of the DK, at
   1 000 000 baud. Logs go to RTT.
#. From the host, read the protocol version with a Spinel frame
   ``81 02 01`` (TID 1, ``CMD_PROP_VALUE_GET``, ``PROP_PROTOCOL_VERSION``),
   HDLC-encoded as ``7E 81 02 01 <FCS> 7E``. The device replies
   ``81 06 01 04 03``: version 4.3.

Differences from standard Spinel
********************************

* ``PROP_INTERFACE_TYPE`` uses a new value, ``0x4E52``, for DECT NR+.
  ``PROP_VENDOR_ID`` is 0. Both are to be agreed upstream.
* Capabilities, properties and statuses use the Spinel vendor ranges.
* The device has no Thread or IEEE 802.15.4 properties: a host that only
  speaks the OpenThread RCP properties gets ``PROP_NOT_FOUND``.
