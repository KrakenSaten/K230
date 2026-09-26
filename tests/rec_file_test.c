/*
 * The recording on disk (tools/recorder/rec_file.h), in a real temporary
 * folder: a complete recording, one that meets a name made meanwhile, one
 * with no audio, a full disk half-way through a write, the WAV size limit,
 * and the repair of every way a writer can disappear - killed after some
 * audio, killed before any, killed with a half frame written, a header that
 * is not a WAV at all, a writer still alive, and one that died in the
 * instant before its .part existed.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pocketwav/pocketwav.h"
#include "rec_file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static int failed;
static int checks;
static char dir[] = "/tmp/rec_file_test.XXXXXX";

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    } else {
        printf("ok   %s\n", what);
    }
}

static int exists(const char *name)
{
    char path[256];
    struct stat st;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return lstat(path, &st) == 0;
}

static int mode_of(const char *name)
{
    char path[256];
    struct stat st;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return stat(path, &st) == 0 ? (int)(st.st_mode & 0777) : -1;
}

static int probe(const char *name, struct pocketwav_info *i)
{
    char path[256];
    int fd;
    int rc;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -100;
    }
    rc = pocketwav_probe_fd(fd, i);
    close(fd);
    return rc;
}

static int count_entries(void)
{
    char cmd[128];
    FILE *p;
    int n = -1;

    snprintf(cmd, sizeof(cmd), "ls -A %s | wc -l", dir);
    p = popen(cmd, "r");
    if (p) {
        if (fscanf(p, "%d", &n) != 1) {
            n = -1;
        }
        pclose(p);
    }
    return n;
}

static void clear_dir(void)
{
    char cmd[128];

    snprintf(cmd, sizeof(cmd), "rm -rf %s/* %s/.[!.]*", dir, dir);
    if (system(cmd) != 0) {
        printf("note could not clear %s\n", dir);
    }
}

static int16_t samples[4000];

static void test_complete(void)
{
    struct rec_file f;
    struct pocketwav_info i;
    char final[REC_NAME_MAX];
    size_t k;
    int ok = 1;

    for (k = 0; k < 4000; k++) {
        samples[k] = (int16_t)(k * 7 - 14000);
    }
    check("a recording starts", rec_file_create(&f, dir, "REC-0001.wav", 16000) == 0);
    check("as REC-0001.wav.part, 0600, and nothing else",
          exists("REC-0001.wav.part") && !exists("REC-0001.wav") && mode_of("REC-0001.wav.part") == 0600 &&
              count_entries() == 1);
    check("which is already a valid, empty WAV", probe("REC-0001.wav.part", &i) == POCKETWAV_OK &&
                                                     i.frames == 0 && i.rate == 16000);
    for (k = 0; k < 4000; k += 1000) {
        ok &= rec_file_append(&f, samples + k, 1000) == 0;
    }
    check("audio is appended", ok && f.data_bytes == 8000);
    check("a checkpoint makes the header say so", rec_file_checkpoint(&f) == 0 &&
                                                      probe("REC-0001.wav.part", &i) == POCKETWAV_OK &&
                                                      i.frames == 4000 && !i.trailing);
    check("finishing renames it", rec_file_finish(&f, final, sizeof(final)) == 0 &&
                                      strcmp(final, "REC-0001.wav") == 0 && exists("REC-0001.wav") &&
                                      !exists("REC-0001.wav.part"));
    check("to a complete WAV of exactly what was written",
          probe("REC-0001.wav", &i) == POCKETWAV_OK && i.frames == 4000 && !i.truncated && !i.trailing &&
              i.file_bytes == 44 + 8000);
    {
        char path[256];
        unsigned char b[8];
        FILE *fp;

        snprintf(path, sizeof(path), "%s/REC-0001.wav", dir);
        fp = fopen(path, "rb");
        check("samples are little-endian, in order",
              fp && fseek(fp, 44, SEEK_SET) == 0 && fread(b, 1, 4, fp) == 4 &&
                  (int16_t)(b[0] | b[1] << 8) == samples[0] && (int16_t)(b[2] | b[3] << 8) == samples[1]);
        if (fp) {
            fclose(fp);
        }
    }
    check("the file is 0600", mode_of("REC-0001.wav") == 0600);
    check("the same name again is refused, and nothing is overwritten",
          rec_file_create(&f, dir, "REC-0001.wav", 16000) == -EEXIST && probe("REC-0001.wav", &i) == 0 &&
              i.frames == 4000);
    check("a name that is not a recording's is refused", rec_file_create(&f, dir, "../x.wav", 16000) == -EINVAL &&
                                                             rec_file_create(&f, dir, "x.wav", 16000) == -EINVAL);
    check("a missing folder is an error, not a crash",
          rec_file_create(&f, "/nonexistent/rec", "REC-0002.wav", 16000) == -ENOENT);
}

static void test_collision(void)
{
    struct rec_file f;
    char final[REC_NAME_MAX];
    char path[256];
    FILE *fp;

    clear_dir();
    rec_file_create(&f, dir, "REC-0005.wav", 48000);
    rec_file_append(&f, samples, 100);
    /* Somebody (Files, a copy) makes a file with our name meanwhile. */
    snprintf(path, sizeof(path), "%s/REC-0005.wav", dir);
    fp = fopen(path, "w");
    if (fp) {
        fputs("theirs", fp);
        fclose(fp);
    }
    check("a name taken during the recording is not replaced",
          rec_file_finish(&f, final, sizeof(final)) == 0 && strcmp(final, "REC-0005-2.wav") == 0 &&
              exists("REC-0005-2.wav"));
    fp = fopen(path, "r");
    {
        char b[8] = "";

        check("theirs is untouched", fp && fgets(b, sizeof(b), fp) && strcmp(b, "theirs") == 0);
    }
    if (fp) {
        fclose(fp);
    }
}

static void test_empty_and_abandon(void)
{
    struct rec_file f;
    char final[REC_NAME_MAX];

    clear_dir();
    rec_file_create(&f, dir, "REC-0010.wav", 16000);
    check("a recording without audio is removed, not saved",
          rec_file_finish(&f, final, sizeof(final)) == -ENODATA && count_entries() == 0 && final[0] == '\0');
    rec_file_create(&f, dir, "REC-0011.wav", 16000);
    rec_file_abandon(&f);
    check("an abandoned empty one leaves nothing", count_entries() == 0);
    rec_file_create(&f, dir, "REC-0012.wav", 16000);
    rec_file_append(&f, samples, 500);
    rec_file_abandon(&f);
    check("an abandoned one with audio stays as a .part for the repair",
          exists("REC-0012.wav.part") && !exists("REC-0012.wav"));
}

static void test_full_disk(void)
{
    struct rec_file f;
    struct pocketwav_info i;
    char final[REC_NAME_MAX];
    int e;

    clear_dir();
    rec_file_create(&f, dir, "REC-0020.wav", 16000);
    rec_file_fail_after = 1001; /* the disk fills in the middle of a frame */
    check("before the disk fills, writes succeed", rec_file_append(&f, samples, 400) == 0);
    e = rec_file_append(&f, samples, 400);
    check("then the write reports ENOSPC", e == -ENOSPC);
    check("and only whole frames that reached the file are counted", f.data_bytes == 1000);
    check("a full disk makes further writes fail too", rec_file_append(&f, samples, 10) == -ENOSPC);
    rec_file_fail_after = -1;
    check("what was written is still saved", rec_file_finish(&f, final, sizeof(final)) == 0 &&
                                                 probe(final, &i) == POCKETWAV_OK && i.frames == 500 &&
                                                 i.file_bytes == 44 + 1000);
}

static void test_limit(void)
{
    struct rec_file f;
    char final[REC_NAME_MAX];

    clear_dir();
    rec_file_create(&f, dir, "REC-0030.wav", 48000);
    /* Pretend six hours have been written: the counter, not the disk. */
    f.data_bytes = POCKETWAV_MAX_DATA_BYTES - 20;
    check("near the WAV limit the room is counted exactly", rec_file_room(&f) == 20);
    check("a write that would pass it is refused whole", rec_file_append(&f, samples, 11) == -EFBIG &&
                                                             f.data_bytes == POCKETWAV_MAX_DATA_BYTES - 20);
    check("a write that fits is taken", rec_file_append(&f, samples, 10) == 0 && rec_file_room(&f) == 0);
    check("and at the limit nothing more", rec_file_append(&f, samples, 1) == -EFBIG);
    f.data_bytes = 10; /* keep the test file small: finish what is really there */
    rec_file_finish(&f, final, sizeof(final));
}

/* A .part as a writer that died would leave it. */
static void dead_part(const char *name, uint32_t header_frames, size_t data_bytes, int garbage_header)
{
    char path[256];
    uint8_t h[44];
    int fd;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (garbage_header) {
        memset(h, 'x', sizeof(h));
    } else {
        pocketwav_header(h, 16000, 1, header_frames * 2);
    }
    if (write(fd, h, 44) != 44 || write(fd, samples, data_bytes) != (ssize_t)data_bytes) {
        printf("note short write\n");
    }
    close(fd);
}

struct seen {
    int repaired;
    int empty;
    int damaged;
    int live;
    char last[REC_NAME_MAX + 8];
};

static void on_seen(enum rec_recover_kind kind, const char *name, void *user)
{
    struct seen *s = user;

    snprintf(s->last, sizeof(s->last), "%s", name);
    s->repaired += kind == REC_RECOVER_REPAIRED;
    s->empty += kind == REC_RECOVER_EMPTY;
    s->damaged += kind == REC_RECOVER_DAMAGED;
    s->live += kind == REC_RECOVER_LIVE;
}

static void test_recover(void)
{
    struct pocketwav_info i;
    struct seen s;
    struct rec_file live;
    char path[256];
    int fd;
    int n;

    clear_dir();
    dead_part("REC-0040.wav.part", 100, 3001, 0); /* header lags, half frame at the end */
    dead_part("REC-0041.wav.part", 0, 0, 0);      /* died before any audio */
    dead_part("REC-0042.wav.part", 0, 400, 1);    /* not a WAV header at all */
    dead_part("song.wav.part", 0, 400, 0);        /* not ours */
    snprintf(path, sizeof(path), "%s/.new-REC-0043.wav.part", dir);
    fd = open(path, O_WRONLY | O_CREAT, 0600);
    close(fd);
    rec_file_create(&live, dir, "REC-0044.wav", 16000); /* a writer still at work */
    rec_file_append(&live, samples, 50);

    memset(&s, 0, sizeof(s));
    n = rec_file_recover_dir(dir, on_seen, &s);
    check("the repair acts on the three dead .parts", n == 3);
    check("one is repaired, as -recovered", s.repaired == 1 && exists("REC-0040-recovered.wav") &&
                                                 !exists("REC-0040.wav.part"));
    check("with every whole frame the file held, and a truthful header",
          probe("REC-0040-recovered.wav", &i) == POCKETWAV_OK && i.frames == 1500 && !i.trailing &&
              !i.truncated && i.file_bytes == 44 + 3000);
    check("one without audio is removed", s.empty == 1 && !exists("REC-0041.wav.part"));
    check("one that is not a WAV is reported and left exactly as it is",
          s.damaged == 1 && exists("REC-0042.wav.part") && !exists("REC-0042-recovered.wav"));
    check("a .part that is not a recording's is not touched", exists("song.wav.part"));
    check("a writer that is still recording is skipped", s.live == 1 && exists("REC-0044.wav.part"));
    check("a stale hidden birth file is swept", !exists(".new-REC-0043.wav.part"));
    check("and the live writer finishes normally afterwards",
          rec_file_finish(&live, NULL, 0) == 0 && exists("REC-0044.wav"));

    /* A second repair of the same name does not replace the first. */
    dead_part("REC-0040.wav.part", 0, 200, 0);
    memset(&s, 0, sizeof(s));
    rec_file_recover_dir(dir, on_seen, &s);
    check("a second repair of the same name takes -2-recovered",
          s.repaired == 1 && exists("REC-0040-2-recovered.wav") && exists("REC-0040-recovered.wav"));
    memset(&s, 0, sizeof(s));
    check("running the repair again finds nothing to do", rec_file_recover_dir(dir, on_seen, &s) == 1 &&
                                                              s.damaged == 1 && s.repaired == 0);
    check("a missing folder is reported", rec_file_recover_dir("/nonexistent/rec", NULL, NULL) == -ENOENT);
}

int main(void)
{
    char cmd[80];

    umask(022); /* the files must be 0600 whatever the umask */
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    test_complete();
    test_collision();
    test_empty_and_abandon();
    test_full_disk();
    test_limit();
    test_recover();
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) {
        printf("note could not remove %s\n", dir);
    }
    printf("rec_file_test: %d checks, %d failure(s)\n", checks, failed);
    return failed != 0;
}
