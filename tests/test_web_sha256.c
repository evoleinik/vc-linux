#include "web_sha256.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(const void *bytes, size_t size, const char *expected) {
    uint8_t digest[32];
    char hex[65];
    web_sha256(bytes, size, digest);
    for (unsigned i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", digest[i]);
    if (strcmp(hex, expected)) {
        fprintf(stderr, "SHA-256 vector failed (%zu bytes): %s != %s\n", size, hex, expected);
        abort();
    }
}

int main(void) {
    check(NULL, 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char message[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    check(message, sizeof message - 1, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    uint8_t *million = malloc(1000000);
    assert(million);
    memset(million, 'a', 1000000);
    check(million, 1000000, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    free(million);
    puts("web SHA-256: empty, abc, two-block padding and million-byte standard vectors passed");
    return 0;
}
