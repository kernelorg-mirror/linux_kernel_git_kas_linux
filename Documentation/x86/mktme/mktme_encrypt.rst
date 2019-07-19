MKTME API: system call encrypt_mprotect()
=========================================

Synopsis
--------
int encrypt_mprotect(void \*addr, size_t len, int prot, key_serial_t serial);

Where *key_serial_t serial* is the serial number of a key allocated
using the MKTME Key Service.

Description
-----------
    encrypt_mprotect() encrypts the memory pages containing any part
    of the address range in the interval specified by addr and len.

    encrypt_mprotect() supports the legacy mprotect() behavior plus
    the enabling of memory encryption. That means that in addition
    to encrypting the memory, the protection flags will be updated
    as requested in the call.

    The *addr* and *len* must be aligned to a page boundary.

    The caller must have *KEY_NEED_VIEW* permission on the key.

    The memory that is to be protected must be mapped *ANONYMOUS*.

Errors
------
    In addition to the Errors returned from legacy mprotect()
    encrypt_mprotect will return:

    ENOKEY *serial* parameter does not represent a valid key.

    EINVAL *len* parameter is not page aligned.

    EACCES Caller does not have *KEY_NEED_VIEW* permission on the key.

EXAMPLE
--------
  Allocate an MKTME Key::
        serial = add_key("mktme", "name", "type=cpu algorithm=aes-xts-128" @u

  Map ANONYMOUS memory::
        ptr = mmap(NULL, size, PROT_NONE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);

  Protect memory::
        ret = syscall(SYS_encrypt_mprotect, ptr, size, PROT_READ|PROT_WRITE,
                      serial);

  Use the encrypted memory

  Free memory::
        ret = munmap(ptr, size);

  Free the key resource::
        ret = keyctl(KEYCTL_INVALIDATE, serial);
