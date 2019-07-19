Overview
=========
Multi-Key Total Memory Encryption (MKTME)[1] is a technology that
allows transparent memory encryption in upcoming Intel platforms.
It uses a new instruction (PCONFIG) for key setup and selects a
key for individual pages by repurposing physical address bits in
the page tables.

Support for MKTME is added to the existing kernel keyring subsystem
and via a new mprotect_encrypt() system call that can be used by
applications to encrypt anonymous memory with keys obtained from
the keyring.

This architecture supports encrypting both normal, volatile DRAM
and persistent memory.  However, persistent memory support is
not included in the Linux kernel implementation at this time.
(We anticipate adding that support next.)

Hardware Background
===================

MKTME is built on top of an existing single-key technology called
TME.  TME encrypts all system memory using a single key generated
by the CPU on every boot of the system. TME provides mitigation
against physical attacks, such as physically removing a DIMM or
watching memory bus traffic.

MKTME enables the use of multiple encryption keys[2], allowing
selection of the encryption key per-page using the page tables.
Encryption keys are programmed into each memory controller and
the same set of keys is available to all entities on the system
with access to that memory (all cores, DMA engines, etc...).

MKTME inherits many of the mitigations against hardware attacks
from TME.  Like TME, MKTME does not mitigate vulnerable or
malicious operating systems or virtual machine managers.  MKTME
offers additional mitigations when compared to TME.

TME and MKTME use the AES encryption algorithm in the AES-XTS
mode.  This mode, typically used for block-based storage devices,
takes the physical address of the data into account when
encrypting each block.  This ensures that the effective key is
different for each block of memory. Moving encrypted content
across physical address results in garbage on read, mitigating
block-relocation attacks.  This property is the reason many of
the discussed attacks require control of a shared physical page
to be handed from the victim to the attacker.

--
1. https://software.intel.com/sites/default/files/managed/a5/16/Multi-Key-Total-Memory-Encryption-Spec.pdf
2. The MKTME architecture supports up to 16 bits of KeyIDs, so a
   maximum of 65535 keys on top of the “TME key” at KeyID-0.  The
   first implementation is expected to support 6 bits, making 63
   keys available to applications.  However, this is not guaranteed.
   The number of available keys could be reduced if, for instance,
   additional physical address space is desired over additional
   KeyIDs.
