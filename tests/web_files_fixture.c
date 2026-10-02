/* Tiny real-Emscripten harness for the production lazy-file code. It does not
 * link a translation or bypass the shared Asyncify download transport. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "embed_lzma.h"
#include "web_files.h"

#define FILE_ROW(n) {"FILE" #n ".TXT", NULL, (n) == 10 ? 0 : 6}
const EmbeddedFile embedded_files[] = {
    FILE_ROW(0), FILE_ROW(1), FILE_ROW(2), FILE_ROW(3), FILE_ROW(4),
    FILE_ROW(5), FILE_ROW(6), FILE_ROW(7), FILE_ROW(8), FILE_ROW(9),
    FILE_ROW(10), FILE_ROW(11), FILE_ROW(12), FILE_ROW(13), FILE_ROW(14), FILE_ROW(15),
};
const int embedded_file_count = sizeof embedded_files / sizeof embedded_files[0];
#define REFERENCE(n, suffix) {"FILE" #n ".TXT", "file." suffix ".bin", NULL, (n) == 10 ? 0 : 6, 0, NULL}
WebFileReference web_file_references[] = {
    REFERENCE(0, "000000000000"), REFERENCE(1, "000000000001"),
    REFERENCE(2, "000000000002"), REFERENCE(3, "000000000003"),
    REFERENCE(4, "000000000004"), REFERENCE(5, "000000000005"),
    REFERENCE(6, "000000000006"), REFERENCE(7, "000000000007"),
    REFERENCE(8, "000000000008"), REFERENCE(9, "000000000009"),
    REFERENCE(10, "000000000010"), REFERENCE(11, "000000000011"),
    REFERENCE(12, "000000000000"), /* an identical immutable reference */
    REFERENCE(13, "000000000013"), REFERENCE(14, "000000000014"),
    REFERENCE(15, "000000000015"),
};
const size_t web_file_reference_count = sizeof web_file_references / sizeof web_file_references[0];

static const char *const hashes[] = {
    "47eaeec42d05db06d398f57df88174d7b47becc63fee4d1d5104ccb554329055",
    "5e38e7e3e6f5369a6c5ebf4b328121d55a17fc576ffeee75e04162eedb9f304e",
    "c5b48de47fbe373c2d4aead75d48a0b20a546e551b8dba2ef6c930744b56a46c",
    "1fccf779ec5e753dbf8d54e0aac62c802c40676e49a5c237c6ff80dc60499977",
    "bf51e2d3dde14c040f8b0067b89710859d88d9f0587ba2ab24eb7655f42c0a46",
    "1f716f7a627ffd894f51cff5ab84052b29c0630018e1f08c9425960479625f44",
    "8d857c697e95c1e06d92bce164e810a31acbe2bbd022f37d15fe4c7c647ef66b",
    "278ab5d10b08297b5fce1fbef6f9b99f6ad73cd8043eef1a159aa6b408897494",
    "1164c9590dae550a84fd466d1728ba56bbd7e1b8ddd6f8a223f35e4afb621cb3",
    "827671854df9b6d67b1d6ba391adc4c9cf0efea827e2dd0db577584574cb6cd3",
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
    "41d30332a9b4fbc3a94b2361a5d8a09baa101fdc4babfe1040db6bf4e03c321c",
    "47eaeec42d05db06d398f57df88174d7b47becc63fee4d1d5104ccb554329055",
    "f1354cda92124ba697cdf30728951a2b6edf0893e3f71a96774c9a227daf8277",
    "9ddfbdf0746ebf228ec66bcf64f499c8f1716921423f4fc2a3357d9a911c4bdf",
    "8a45889a04ea6ec88ec6c36ddf3bad7817ec97693bb6cf4ea1ae7c7cd66281fd",
};

static void expected(int index, uint8_t bytes[6])
{
    memcpy(bytes, "Abytes", 6);
    if (index != 12) bytes[0] += index;
}

void fixture_init(void)
{
    mkdir("/var", 0777);
    mkdir("/var/vc", 0777);
    for (size_t index = 0; index < web_file_reference_count; index++) {
        uint8_t bytes[6];
        expected((int)index, bytes);
        web_file_references[index].sha256 = hashes[index];
        web_file_references[index].checksum = embed_adler32(bytes, web_file_references[index].size);
    }
}

int fixture_install(int index, const char *path)
{
    if (index < 0 || index >= embedded_file_count) return -1;
    return web_files_install(path, embedded_files + index);
}

int fixture_open(const char *path, int action)
{
    int flags = action == 1 ? O_WRONLY : action == 2 ? O_RDWR :
        action == 3 ? O_WRONLY | O_TRUNC : action == 4 ? O_WRONLY | O_CREAT | O_EXCL : O_RDONLY;
    int fd = web_files_open(path, flags, 0666);
    if (fd < 0) return errno == ENOENT ? 2 : errno == ENOMEM ? 8 : errno == EEXIST ? 80 : 5;
    close(fd);
    return 0;
}

int fixture_reference(int index)
{
    if (index < 0 || index >= embedded_file_count) return -1;
    const uint8_t *bytes;
    int error = web_files_reference(embedded_files + index, &bytes);
    if (error) return error;
    uint8_t original[6];
    expected(index, original);
    return memcmp(bytes, original, embedded_files[index].size) ? -1 : 0;
}
