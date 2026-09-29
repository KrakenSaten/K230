/*
 * Video's folder list: which names count, the folder's location from the
 * environment, sorting, sizes, the bound on what is shown and read, and the
 * time and size texts.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "video_files.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks;
static int failed;

static void check(const char *name, int ok)
{
    checks++;
    if (ok) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failed++;
    }
}

static void touch_file(const char *dir, const char *name, size_t bytes)
{
    char path[512];
    int fd;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        if (bytes && ftruncate(fd, (off_t)bytes) != 0) {
            perror("ftruncate");
        }
        close(fd);
    }
}

int main(void)
{
    char tmpl[] = "/tmp/video_files_test.XXXXXX";
    char *dir = mkdtemp(tmpl);
    char out[512];
    char path[512];
    struct video_files *list = calloc(1, sizeof(*list));
    int i;

    if (!dir || !list) {
        perror("setup");
        return 1;
    }

    check("clip.mp4 plays", video_files_playable_name("clip.mp4"));
    check("CLIP.MP4 plays", video_files_playable_name("CLIP.MP4"));
    check("a name with spaces plays", video_files_playable_name("my holiday.mp4"));
    check("hidden files do not", !video_files_playable_name(".clip.mp4"));
    check("other containers are not claimed", !video_files_playable_name("clip.mkv") &&
                                                  !video_files_playable_name("clip.mov") &&
                                                  !video_files_playable_name("clip.avi"));
    check("a bare .mp4 is not a name", !video_files_playable_name(".mp4"));
    check("no extension", !video_files_playable_name("clip"));
    check("mp4 inside the name only", !video_files_playable_name("clip.mp4.part"));
    check("control characters", !video_files_playable_name("cl\nip.mp4") &&
                                    !video_files_playable_name("cl\x7fip.mp4"));
    {
        char longname[VIDEO_FILES_NAME_MAX + 8];

        memset(longname, 'a', sizeof(longname));
        strcpy(longname + VIDEO_FILES_NAME_MAX, ".mp4");
        check("a name too long for the line protocol", !video_files_playable_name(longname));
    }
    check("NULL and empty", !video_files_playable_name(NULL) && !video_files_playable_name(""));

    /* The folder. */
    setenv("POCKETOS_VIDEOS_DIR", "/data/videos", 1);
    check("POCKETOS_VIDEOS_DIR wins", video_files_dir(out, sizeof(out)) == 0 &&
                                          strcmp(out, "/data/videos") == 0);
    setenv("POCKETOS_VIDEOS_DIR", "relative/videos", 1);
    setenv("HOME", "/home/u", 1);
    check("a relative override is ignored: HOME/Videos",
          video_files_dir(out, sizeof(out)) == 0 && strcmp(out, "/home/u/Videos") == 0);
    unsetenv("POCKETOS_VIDEOS_DIR");
    setenv("HOME", "/", 1);
    check("HOME=/ gives /root/Videos", video_files_dir(out, sizeof(out)) == 0 &&
                                           strcmp(out, "/root/Videos") == 0);
    unsetenv("HOME");
    check("no HOME gives /root/Videos", video_files_dir(out, sizeof(out)) == 0 &&
                                            strcmp(out, "/root/Videos") == 0);
    check("a buffer too small is refused", video_files_dir(out, 5) == -1);

    /* Scanning. */
    touch_file(dir, "b.mp4", 2500000);
    touch_file(dir, "a.mp4", 1000);
    touch_file(dir, "C.MP4", 0);
    touch_file(dir, "notes.txt", 10);
    touch_file(dir, ".hidden.mp4", 10);
    snprintf(path, sizeof(path), "%s/folder.mp4", dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/link.mp4", dir);
    if (symlink("a.mp4", path) != 0) {
        perror("symlink");
    }
    snprintf(path, sizeof(path), "%s/dangling.mp4", dir);
    if (symlink("nowhere.mp4", path) != 0) {
        perror("symlink");
    }
    check("four playable files (a link followed, a directory, a dangling link and the rest not)",
          video_files_scan(list, dir) == 4 && list->count == 4 && !list->more && list->error == 0);
    check("sorted byte-wise", strcmp(list->items[0].name, "C.MP4") == 0 &&
                                  strcmp(list->items[1].name, "a.mp4") == 0 &&
                                  strcmp(list->items[2].name, "b.mp4") == 0 &&
                                  strcmp(list->items[3].name, "link.mp4") == 0);
    check("sizes", list->items[1].bytes == 1000 && list->items[2].bytes == 2500000 &&
                       list->items[3].bytes == 1000);

    snprintf(path, sizeof(path), "%s/none", dir);
    check("a missing folder says ENOENT", video_files_scan(list, path) == -1 &&
                                              list->error == ENOENT && list->count == 0);

    for (i = 0; i < VIDEO_FILES_MAX + 5; i++) {
        char n[32];

        snprintf(n, sizeof(n), "f%04d.mp4", i);
        touch_file(dir, n, 0);
    }
    check("the list stops at VIDEO_FILES_MAX and says there is more",
          video_files_scan(list, dir) == VIDEO_FILES_MAX && list->count == VIDEO_FILES_MAX &&
              list->more);

    /* Texts. */
    video_files_size_text(0, out, sizeof(out));
    check("0 KB", strcmp(out, "0 KB") == 0);
    video_files_size_text(850000, out, sizeof(out));
    check("850 KB", strcmp(out, "850 KB") == 0);
    video_files_size_text(3460983, out, sizeof(out));
    check("3.5 MB", strcmp(out, "3.5 MB") == 0);
    video_files_size_text(1000000000ull, out, sizeof(out));
    check("1000.0 MB", strcmp(out, "1000.0 MB") == 0);
    video_files_time_text(0, out, sizeof(out));
    check("0:00", strcmp(out, "0:00") == 0);
    video_files_time_text(-5, out, sizeof(out));
    check("negative reads as 0:00", strcmp(out, "0:00") == 0);
    video_files_time_text(18200, out, sizeof(out));
    check("0:18", strcmp(out, "0:18") == 0);
    video_files_time_text(3599999, out, sizeof(out));
    check("59:59", strcmp(out, "59:59") == 0);
    video_files_time_text(3723000, out, sizeof(out));
    check("1:02:03", strcmp(out, "1:02:03") == 0);

    {
        char cmd[600];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
        if (system(cmd) != 0) {
            fprintf(stderr, "cleanup failed\n");
        }
    }
    free(list);
    printf("video_files_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
