/* A trailing slash on the name a call creates or replaces is a statement the
 * kernel acts on, and the emulator's pin of the final component dropped it:
 *   - open(O_CREAT) of "f/" is EISDIR before any lookup (open_last_lookups);
 *   - filename_create (mkdir, mknod, symlink, link's new name): EEXIST for a
 *     name that is there, ENOENT for one that is not unless the call is mkdir;
 *   - rename of a non-directory onto "x/" is ENOTDIR (do_renameat2).
 * Each wrong answer but the first made the file under the slash-less name. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static void say(const char *what, long r) {
    printf("%s=%d\n", what, r < 0 ? errno : 0);
}

int main(void) {
    char dir[] = "/tmp/slashcreateXXXXXX";
    if (!mkdtemp(dir) || chdir(dir) != 0) return 1;
    close(open("f", O_WRONLY | O_CREAT, 0644));
    mkdir("d", 0755);
    if (symlink("nowhere", "dang")) return 1;
    say("creat_file_slash", open("f/", O_WRONLY | O_CREAT, 0644));
    say("creat_new_slash", open("new/", O_WRONLY | O_CREAT, 0644));
    say("creat_dir_slash", open("d/", O_WRONLY | O_CREAT, 0644));
    say("rename_dst_slash", rename("f", "moved/"));
    say("rename_dir_dst_slash", rename("d", "d3/"));
    say("rename_dir_back", rename("d3", "d"));
    say("link_dst_slash", link("f", "lk/"));
    say("symlink_dst_slash", symlink("f", "sl/"));
    say("symlink_dang_slash", symlink("f", "dang/"));
    say("mknod_dst_slash", mknod("fifo/", S_IFIFO | 0644, 0));
    say("mkdir_over_file_slash", mkdir("f/", 0755));
    say("mkdir_over_dang_slash", mkdir("dang/", 0755));
    say("mkdir_new_slash", mkdir("nd/", 0755));
    struct stat st;
    printf("f_kept=%d moved=%d lk=%d sl=%d fifo=%d\n", stat("f", &st) == 0,
           lstat("moved", &st) == 0, lstat("lk", &st) == 0, lstat("sl", &st) == 0,
           lstat("fifo", &st) == 0);
    unlink("f"); unlink("dang"); rmdir("d"); rmdir("nd");
    if (chdir("/")) return 1;
    rmdir(dir);
    return 0;
}
