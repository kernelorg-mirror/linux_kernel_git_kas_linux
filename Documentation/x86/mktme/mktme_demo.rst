Demonstration Program using MKTME API's
=======================================

/* Compile with the keyutils library: cc -o mdemo mdemo.c -lkeyutils */

#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <keyutils.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define PAGE_SIZE sysconf(_SC_PAGE_SIZE)
#define sys_encrypt_mprotect 434

void main(void)
{
	char *options_CPU = "algorithm=aes-xts-128 type=cpu";
	long size = PAGE_SIZE;
        key_serial_t key;
	void *ptra;
	int ret;

        /* Allocate an MKTME Key */
	key = add_key("mktme", "testkey", options_CPU, strlen(options_CPU),
                      KEY_SPEC_THREAD_KEYRING);

	if (key == -1) {
		printf("addkey FAILED\n");
		return;
	}
        /* Map a page of ANONYMOUS memory */
	ptra = mmap(NULL, size, PROT_NONE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
	if (!ptra) {
		printf("failed to mmap");
		goto inval_key;
	}
        /* Encrypt that page of memory with the MKTME Key */
	ret = syscall(sys_encrypt_mprotect, ptra, size, PROT_NONE, key);
	if (ret)
		printf("mprotect error [%d]\n", ret);

        /* Enjoy that page of encrypted memory */

        /* Free the memory */
	ret = munmap(ptra, size);

inval_key:
        /* Free the Key */
	if (keyctl(KEYCTL_INVALIDATE, key) == -1)
		printf("invalidate failed on key [%d]\n", key);
}
