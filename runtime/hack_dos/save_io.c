/* Validate persistent saves before upstream's pointer-rich recovery begins.
 * The first level header covers the entire file. Internal levels carry the
 * same format/build marker, with zero length/checksum until made persistent.
 */
#include <fcntl.h>
#include <io.h>
#include "hack.h"
#include "extern.h"

struct save_header {
    unsigned char magic[4];
    unsigned long build;
    unsigned long length;
    unsigned long checksum;
};
typedef char save_header_must_be_16[(sizeof(struct save_header) == 16) ? 1 : -1];
static const unsigned char save_magic[4] = {'H','A','C','K'};
extern unsigned long hack_build_stamp;

int hack_creat(const char *path, int mode)
{
    /* Reading back the saved bytes catches short writes before accepting the
     * file as resumable, and permits its integrity header to be finalized. */
    return open(path, O_CREAT | O_TRUNC | O_RDWR | O_BINARY, mode);
}
void hack_write_header(int fd)
{
    struct save_header header;
    memcpy(header.magic, save_magic, sizeof(save_magic));
    header.build = hack_build_stamp;
    header.length = header.checksum = 0;
    bwrite(fd, &header, sizeof(header));
}
void hack_read_header(int fd)
{
    struct save_header header;
    mread(fd, &header, sizeof(header));
    if (memcmp(header.magic, save_magic, sizeof(save_magic)) ||
        header.build != hack_build_stamp)
        error("Incompatible Hack level file.");
}

static int checksum_file(int fd, unsigned long length, unsigned long *result)
{
    unsigned char bytes[512];
    unsigned request, i;
    int got;
    unsigned long remaining, checksum = 2166136261UL;
    if (length < sizeof(struct save_header)) return 0;
    if (lseek(fd, sizeof(struct save_header), SEEK_SET) < 0) return 0;
    remaining = length - sizeof(struct save_header);
    while (remaining) {
        request = remaining > sizeof(bytes) ? sizeof(bytes) : (unsigned)remaining;
        got = read(fd, bytes, request);
        if (got != request) return 0;
        for (i = 0; i < request; ++i)
            checksum = (checksum ^ bytes[i]) * 16777619UL;
        remaining -= request;
    }
    *result = checksum;
    return 1;
}

int hack_finish_file(int fd)
{
    struct save_header header;
    long length = filelength(fd);
    if (length < sizeof(header)) return 0;
    memcpy(header.magic, save_magic, sizeof(save_magic));
    header.build = hack_build_stamp;
    header.length = length;
    if (!checksum_file(fd, header.length, &header.checksum)) return 0;
    if (lseek(fd, 0L, SEEK_SET) < 0) return 0;
    if (write(fd, &header, sizeof(header)) != sizeof(header)) return 0;
    return 1;
}

/* A negative result distinguishes a different executable/format from a
 * damaged current-build file. Both are refused before pointer recovery;
 * only the latter remains available in the corruption quarantine. */
static int check_file(int fd)
{
    struct save_header header;
    unsigned long checksum;
    int valid = 0;
    if (lseek(fd, 0L, SEEK_SET) >= 0 &&
        read(fd, &header, sizeof(header)) == sizeof(header)) {
        if (memcmp(header.magic, save_magic, sizeof(save_magic)) ||
            header.build != hack_build_stamp) {
            /* The old HACK103/1 header also mismatches this build stamp. */
            pline("Saved level is out of date. ");
            valid = -1;
        } else if (header.length >= sizeof(levl) + 128UL &&
                   header.length == (unsigned long)filelength(fd) &&
                   checksum_file(fd, header.length, &checksum) &&
                   checksum == header.checksum)
            valid = 1;
    }
    if (lseek(fd, 0L, SEEK_SET) < 0 && valid > 0) valid = 0;
    return valid;
}

int uptodate(int fd)
{
    return check_file(fd) > 0;
}

int hack_resume_valid(int fd)
{
    int valid = check_file(fd);
    if (valid > 0) return 1;
    close(fd);
    if (valid < 0) {
        /* Upstream discards outdated saves and getbones discards outdated
         * bones after uptodate(). Close first for DOS unlink semantics. */
        (void)unlink(SAVEF);
        return 0;
    }
    if (rename(SAVEF, "HACK.BAD") == 0)
        printf("Invalid save renamed to HACK.BAD; starting a new game.\n");
    else
        printf("Invalid save left in HACK.SAV; starting a new game.\n");
    return 0;
}
