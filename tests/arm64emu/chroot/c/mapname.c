/* /proc/self/maps names a mapped file by its path, device and inode
 * (show_map_vma). The emulator printed plain file mappings as anonymous
 * memory -- no name, 00:00 0 -- so a program looking itself up there (its
 * own library, a mapped database) found nothing; and a long path is printed
 * whole. Paths are compared, never printed: the directory differs per run. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static void check(const char *label, const char *path) {
    int fd = open(path, O_RDONLY);
    struct stat st;
    fstat(fd, &st);
    char *p = mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    char key[32];
    snprintf(key, sizeof key, "%lx-", (unsigned long)p);
    FILE *f = fopen("/proc/self/maps", "r");
    static char line[16384];
    int found = 0, named = 0, ids = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, strlen(key))) continue;
        found = 1;
        line[strcspn(line, "\n")] = 0;
        char *nm = strchr(line, '/');
        named = nm && !strcmp(nm, path);
        unsigned ma, mi;
        unsigned long long ino;
        if (sscanf(line, "%*s %*s %*s %x:%x %llu", &ma, &mi, &ino) == 3)
            ids = ma == major(st.st_dev) && mi == minor(st.st_dev) && ino == st.st_ino;
    }
    fclose(f);
    printf("%s: found=%d named=%d dev_ino=%d\n", label, found, named, ids);
    munmap(p, 4096);
}

int main(void) {
    char dir[] = "/tmp/mapnameXXXXXX";
    if (!mkdtemp(dir)) return 1;
    char path[3000];
    snprintf(path, sizeof path, "%s/f", dir);
    FILE *f = fopen(path, "w");
    fputs("x", f);
    fclose(f);
    check("short", path);
    /* A path past a page of text. */
    char deep[3000];
    snprintf(deep, sizeof deep, "%s", dir);
    for (int i = 0; i < 40; i++) {
        strcat(deep, "/deepdeepdeepdeepdeepdeepdeepdeepdeepdeepdeepdeepdeep");
        mkdir(deep, 0700);
    }
    char df[3100];
    snprintf(df, sizeof df, "%s/g", deep);
    f = fopen(df, "w");
    fputs("y", f);
    fclose(f);
    check("long", df);
    unlink(df);
    for (int i = 0; i < 40; i++) {
        rmdir(deep);
        *strrchr(deep, '/') = 0;
    }
    unlink(path);
    rmdir(dir);
    return 0;
}
