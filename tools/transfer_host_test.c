/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 传书公共实现的主机回归。/ Host regression of the shared transfer implementation.
 */
#define WIFI_TRANSFER_HOST_TEST
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
static char stat_alias[448], stat_target[448];
static int test_stat(const char *path, struct stat *out) {
    return stat(stat_alias[0] && !strcmp(path, stat_alias) ? stat_target : path, out);
}
static int fail_write, fail_close, fatfs_replace, fail_commit, fail_remove, fail_restore;
static int test_remove(const char *path) {
    if (fail_remove) { errno = EACCES; return -1; }
    return remove(path);
}
static size_t test_write(const void *p, size_t size, size_t n, FILE *f) {
    if (fail_write) return 0;
    return fwrite(p, size, n, f);
}
static int test_close(FILE *f) {
    int result = fclose(f);
    return fail_close ? EOF : result;
}
static int test_rename(const char *from, const char *to) {
    if (fail_restore && strstr(from, ".rename-backup")) { errno = EIO; return -1; }
    if (strstr(from, ".part") || strstr(from, ".pico-upload-")) {
        FILE *old = fopen(to, "rb");
        if (old) { fclose(old); if (fatfs_replace) { errno = EEXIST; return -1; } }
        if (fail_commit) { errno = EIO; return -1; }
    }
    return rename(from, to);
}
#define fwrite test_write
#define fclose test_close
#define rename test_rename
#define remove test_remove
#define stat(path, out) test_stat(path, out)
#include "../components/wifi_transfer/wifi_transfer.c"
#undef fwrite
#undef fclose
#undef rename
#undef remove
#undef stat
#include <assert.h>
#include <unistd.h>

static int remaining, fail_recv, fail_after, receive_calls;
static int receive(void *ctx, char *buf, size_t n) {
    (void)ctx;
    if (fail_recv) return -1;
    if (fail_after && ++receive_calls == fail_after) return -1;
    if (n > (size_t)remaining) n = remaining;
    memset(buf, 'a', n); remaining -= n; return (int)n;
}
static void write_fixture(const char *path, const char *text) {
    FILE *f = fopen(path, "wb"); assert(f);
    assert(fwrite(text, 1, strlen(text), f) == strlen(text)); assert(fclose(f) == 0);
}
static int cleanup_calls, cleanup_failure;
static char cleaned_path[448];
static int change_callback(const char *path) {
    assert(path[0]); snprintf(cleaned_path, sizeof(cleaned_path), "%s", path);
    ++cleanup_calls; return cleanup_failure;
}
// 任意文件使用实际接收/提交代码，验证路径、字节和失败后的旧文件保留。
// General files use the actual receive/commit code to verify paths, bytes and old-file preservation on failure.
static void test_directory_upload(void) {
    char root[] = "/tmp/transfer-files-XXXXXX", target[256], temp[256], relative[240], absolute[256], buf[17];
    assert(mkdtemp(root));
    assert(sd_decode_relative("books/%E8%B5%84%E6%96%99%20%25%2B%23.bin", relative, false));
    assert(!strcmp(relative, "books/资料 %+#.bin"));
    assert(sd_absolute(relative, absolute, sizeof(absolute)) && !strcmp(absolute, "/sdcard/books/资料 %+#.bin"));
    for (const char **p = (const char *[]){"%2E%2E/x", "books/../x", "%2Fetc/x", "books/%00x", "x%5Cy", "x/", "books//x", "x%3Ay", NULL}; *p; ++p)
        assert(!sd_decode_relative(*p, relative, false));
    assert(sd_decode_relative("", relative, true) && !sd_decode_relative("", relative, false));
    char long_path[240];
    memcpy(long_path, "books/", 6);
    memset(long_path + 6, 'a', 233);
    long_path[239] = 0;
    assert(!sd_decode_relative(long_path, relative, false));
    long_path[238] = 0;
    assert(sd_decode_relative(long_path, relative, false) && strlen(relative) == 238);
    snprintf(target, sizeof(target), "%s/books", root); assert(!mkdir(target, 0700));
    remaining = 53;
    file_result_t r = upload_directory_file(root, "books/资料 %+#.bin", 53, 53, false, buf, sizeof(buf), receive, NULL, NULL, change_callback);
    assert(r.status == 200 && r.changed && remaining == 0 && !cleanup_calls);
    snprintf(target, sizeof(target), "%s/books/资料 %%+#.bin", root);
    FILE *f = fopen(target, "rb"); assert(f);
    for (int i = 0; i < 53; ++i) assert(fgetc(f) == 'a');
    assert(fgetc(f) == EOF && !fclose(f));
    r = upload_directory_file(root, "books/资料 %+#.bin", 12, 100, false, buf, sizeof(buf), receive, NULL, NULL, change_callback);
    assert(r.status == 409 && !r.changed);
    fail_after = 2; receive_calls = 0; remaining = 53;
    r = upload_directory_file(root, "books/资料 %+#.bin", 53, 100, true, buf, sizeof(buf), receive, NULL, NULL, change_callback);
    assert(r.status == 408 && !r.changed); fail_after = 0;
    struct stat st; assert(!stat(target, &st) && st.st_size == 53);
    fatfs_replace = 1; remaining = 9;
    r = upload_directory_file(root, "books/资料 %+#.bin", 9, 100, true, buf, sizeof(buf), receive, NULL, NULL, change_callback);
    assert(r.status == 200 && r.changed && !stat(target, &st) && st.st_size == 9);
    fatfs_replace = 0; assert(!remove(target));
    r = upload_directory_file(root, "../escape", 9, 100, false, buf, sizeof(buf), receive, NULL, NULL, NULL);
    assert(r.status == 400 && !r.changed);
    r = upload_directory_file(root, "missing/file", 9, 100, false, buf, sizeof(buf), receive, NULL, NULL, NULL);
    assert(r.status == 404 && !r.changed);
    r = upload_directory_file(root, "large.zip", 101, 100, false, buf, sizeof(buf), receive, NULL, NULL, NULL);
    assert(r.status == 507 && !r.changed);
    r = upload_directory_file(root, "books", 9, 100, false, buf, sizeof(buf), receive, NULL, NULL, NULL);
    assert(r.status == 400 && !r.changed);
    // 无扩展名、点开头及零字节文件都可以上传。/ Extensionless, dot-prefixed and empty files are accepted.
    for (const char **p = (const char *[]){"README", ".env", "Pico-update.bin", NULL}; *p; ++p) {
        r = upload_directory_file(root, *p, 0, 0, false, buf, sizeof(buf), receive, NULL, NULL, change_callback);
        assert(r.status == 200 && r.changed && !cleanup_calls);
        snprintf(target, sizeof(target), "%s/%s", root, *p);
        assert(!stat(target, &st) && st.st_size == 0 && !remove(target));
    }
    snprintf(temp, sizeof(temp), "%s/books/test.txt.part", root); write_fixture(temp, "user file");
    remaining = 4;
    r = upload_directory_file(root, "books/test.txt", 4, 100, false, buf, sizeof(buf), receive, NULL, NULL, change_callback);
    assert(r.status == 200 && cleanup_calls == 1 && !stat(temp, &st) && st.st_size == 9);
    assert(!remove(temp)); snprintf(target, sizeof(target), "%s/books/test.txt", root); assert(!remove(target));
    snprintf(target, sizeof(target), "%s/books", root);
    DIR *dir = opendir(target); assert(dir); struct dirent *entry; unsigned entries = 0;
    while ((entry = readdir(dir))) if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) ++entries;
    assert(!entries && !closedir(dir) && !rmdir(target) && !rmdir(root));
    cleanup_calls = 0; cleaned_path[0] = 0;
}
static void test_long_book_upload(void) {
    assert(book_upload_name_limit("/sdcard/books", false) == 232);
    assert(book_upload_name_limit("/sdcard/books/sub", false) == 228);
    assert(book_upload_name_limit("/flash/books", true) == 120);
    char name[234], decoded[233], encoded[700];
    for (size_t i = 0; i < 75; ++i) memcpy(name + i * 3, "书", 3);
    strcpy(name + 225, "%a.epub");
    assert(strlen(name) == 232);
    for (size_t i = 0; i < 232; ++i) snprintf(encoded + i * 3, 4, "%%%02X", (unsigned char)name[i]);
    assert(decode_typed_name(encoded, decoded, sizeof(decoded), 0) == NAME_OK && !strcmp(decoded, name));
    assert(decode_typed_name(encoded, decoded, 121, 0) == NAME_TOO_LONG);
    strcpy(encoded + 696, "%61");
    assert(decode_typed_name(encoded, decoded, sizeof(decoded), 0) == NAME_TOO_LONG);
    assert(decode_typed_name("bad%00.epub", decoded, sizeof(decoded), 0) == NAME_INVALID);
    char reserved[200];
    snprintf(reserved, sizeof(reserved), "books/.MOGO-U-%0120d.txt.PART", 0);
    assert(!sd_relative_valid(reserved, false));
    assert(sd_relative_valid("books/.mogo-u-user.epub.part", false));

    char root[] = "/tmp/transfer-long-XXXXXX", path[320], part[320], backup[320], user_part[320], user_backup[320], buf[16];
    assert(mkdtemp(root));
    snprintf(path, sizeof(path), "%s/%s", root, name);
    snprintf(user_part, sizeof(user_part), "%s/%s" PART_SUFFIX, root, name);
    snprintf(user_backup, sizeof(user_backup), "%s/%s" BACKUP_SUFFIX, root, name);
    assert(staged_book_path(root, name, PART_SUFFIX, part, sizeof(part)));
    assert(staged_book_path(root, name, BACKUP_SUFFIX, backup, sizeof(backup)));
    write_fixture(user_part, "user file");
    write_fixture(user_backup, "user backup");
    unsigned removed, restored;
    assert(cleanup_interrupted(root, TRANSFER_BOOK_NAME_MAX, &removed, &restored) == 0);
    assert(!removed && !restored && access(path, F_OK) != 0);
    assert(access(user_part, F_OK) == 0 && access(user_backup, F_OK) == 0);
    write_fixture(part, "unexpected collision"); remaining = 8;
    int before = cleanup_calls;
    assert(upload_managed(path, part, backup, 8, 0, 100, false, buf, sizeof(buf), receive, NULL, NULL, change_callback).status == 507);
    assert(remaining == 8 && cleanup_calls == before && access(part, F_OK) == 0);
    unlink(part);
    assert(upload_managed(path, part, backup, 8, 0, 100, false, buf, sizeof(buf), receive, NULL, NULL, change_callback).changed);
    assert(!strcmp(cleaned_path, path) && cleanup_calls == before + 1);
    remaining = 12;
    assert(upload_managed(path, part, backup, 12, 0, 100, false, buf, sizeof(buf), receive, NULL, NULL, change_callback).status == 409);
    assert(remaining == 12 && cleanup_calls == before + 1);
    fail_recv = 1;
    assert(upload_managed(path, part, backup, 12, 0, 100, true, buf, sizeof(buf), receive, NULL, NULL, change_callback).status == 408);
    fail_recv = 0;
    struct stat st;
    assert(!stat(path, &st) && st.st_size == 8 && access(part, F_OK) != 0);
    fatfs_replace = fail_commit = fail_restore = 1;
    assert(upload_managed(path, part, backup, 12, 0, 100, true, buf, sizeof(buf), receive, NULL, NULL, change_callback).status == 507);
    assert(cleanup_calls == before + 1 && access(path, F_OK) != 0 && access(part, F_OK) == 0 && access(backup, F_OK) == 0);
    fail_commit = fail_restore = 0;
    assert(cleanup_interrupted(root, TRANSFER_BOOK_NAME_MAX, &removed, &restored) == 0 && removed == 1 && restored == 1);
    assert(!stat(path, &st) && st.st_size == 8 && access(part, F_OK) != 0 && access(backup, F_OK) != 0);
    remaining = 12;
    assert(upload_managed(path, part, backup, 12, 0, 100, true, buf, sizeof(buf), receive, NULL, NULL, change_callback).changed);
    assert(!stat(path, &st) && st.st_size == 12 && cleanup_calls == before + 2);
    fatfs_replace = 0;
    unlink(path);
    write_fixture(backup, "backup from an already committed upload");
    assert(cleanup_interrupted(root, TRANSFER_BOOK_NAME_MAX, &removed, &restored) == 0 && !restored);
    assert(access(path, F_OK) != 0 && access(backup, F_OK) == 0);
    assert(access(user_part, F_OK) == 0 && access(user_backup, F_OK) == 0);
    unlink(backup); unlink(user_part); unlink(user_backup); assert(!rmdir(root));
    cleanup_calls = before;
}

int main(void) {
    test_long_book_upload();
    test_directory_upload();
    char alias_root[] = "/tmp/transfer-alias-XXXXXX", canonical[448], requested[448];
    assert(mkdtemp(alias_root));
    snprintf(canonical, sizeof(canonical), "%s/Book.txt", alias_root);
    write_fixture(canonical, "original");
    assert(resolve_mutation_path(alias_root, "book.txt", requested, sizeof(requested)) == 0);
    assert(!strcmp(canonical, requested));
    char flash_root[] = "/tmp/transfer-flash-XXXXXX", flash_canonical[448];
    assert(mkdtemp(flash_root));
    snprintf(flash_canonical, sizeof(flash_canonical), "%s/FlashBook.txt", flash_root);
    write_fixture(flash_canonical, "flash");
    assert(resolve_mutation_path(flash_root, "flashbook.TXT", requested, sizeof(requested)) == 0);
    assert(!strcmp(flash_canonical, requested));
    unlink(flash_canonical); rmdir(flash_root);
    snprintf(stat_alias, sizeof(stat_alias), "%s/BOOK~1.TXT", alias_root);
    strcpy(stat_target, canonical);
    assert(resolve_mutation_path(alias_root, "BOOK~1.TXT", requested, sizeof(requested)) == 400);
    snprintf(stat_alias, sizeof(stat_alias), "%s/BÖÖK.TXT", alias_root);
    assert(resolve_mutation_path(alias_root, "BÖÖK.TXT", requested, sizeof(requested)) == 400);
    stat_alias[0] = 0;
    assert(resolve_mutation_path(alias_root, "book.txt", requested, sizeof(requested)) == 0);
    assert(!strcmp(canonical, requested));
    char alias_backup[480]; snprintf(alias_backup, sizeof(alias_backup), "%s.rename-backup", canonical);
    assert(rename(canonical, alias_backup) == 0);
    assert(resolve_mutation_path(alias_root, "BOOK.TXT", requested, sizeof(requested)) == 0);
    assert(!strcmp(canonical, requested));
    snprintf(stat_alias, sizeof(stat_alias), "%s/BOOK~1.TXT.rename-backup", alias_root);
    strcpy(stat_target, alias_backup);
    assert(resolve_mutation_path(alias_root, "BOOK~1.TXT", requested, sizeof(requested)) == 400);
    stat_alias[0] = 0;
    unlink(alias_backup); rmdir(alias_root);
    assert(name_contains("红楼梦.txt", "hlm"));
    assert(name_contains("重庆故事.epub", "chongqing"));
    assert(name_contains("Harry Potter.txt", "hp"));
    assert(!name_contains("红楼梦.txt", "txt"));
    char name[121], root[] = "/tmp/transfer-test-XXXXXX", path[256], part[264], buf[16];
    assert(mkdtemp(root));
    assert(decode_name_limit("%E4%B8%AD%E6%96%87.epub", name, sizeof(name), 0));
    assert(!strcmp(name, "中文.epub"));
    assert(decode_name_limit("%E4%B8%AD%E6%96%87.MOBI", name, sizeof(name), 0) && !strcmp(name, "中文.MOBI"));
    assert(!decode_name_limit("bad.mobi.exe", name, sizeof(name), 0));
    char mobi_path[448]; snprintf(mobi_path, sizeof(mobi_path), "%s/test.MOBI", root);
    write_fixture(mobi_path, "mobi");
    transfer_book_page_t mobi_listing;
    assert(list_books(root, "test", 0, &mobi_listing) == 0 && mobi_listing.total == 1 && mobi_listing.items[0].size == 4);
    assert(delete_managed(mobi_path, change_callback).changed && cleanup_calls == 1);
    char long_mobi[240]; memset(long_mobi, 'x', 214); strcpy(long_mobi + 214, ".mobi");
    remaining = 4;
    assert(upload_directory_file(root, long_mobi, 4, 100, false, buf, sizeof(buf), receive, NULL, NULL, change_callback).changed);
    assert(cleanup_calls == 2);
    snprintf(mobi_path, sizeof(mobi_path), "%s/%s", root, long_mobi);
    assert(delete_managed(mobi_path, change_callback).changed && cleanup_calls == 3);
    cleanup_calls = 0;
    const char *bad[] = {"../a.txt", "%2e%2e.txt", "a%2fb.txt", "a%5cb.txt", "a%00.txt", "a%ff.txt", "a%C0%AF.txt", "a%ED%A0%80.txt", "a%F4%90%80%80.txt", "a%.txt", "a.pdf", "a.txt.", ".txt", "a:book.txt"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(*bad); ++i) assert(!decode_name_limit(bad[i], name, sizeof(name), 0));
    char longname[128]; memset(longname, 'a', 117); strcpy(longname + 117, ".txt");
    assert(!decode_name_limit(longname, name, sizeof(name), 0));
    memset(longname, 'a', 116); strcpy(longname + 116, ".TXT");
    assert(decode_name_limit(longname, name, sizeof(name), 0) && strlen(name) == 120);
    strcpy(longname + 116, ".jpg");
    assert(decode_name_limit(longname, name, sizeof(name), 1) && strlen(name) == 120);
    strcpy(longname + 116, ".jpeg");
    assert(!decode_name_limit(longname, name, sizeof(name), 1));
    strcpy(longname + 116, ".otf");
    assert(decode_name_limit(longname, name, sizeof(name), 2) && strlen(name) == 120);
    longname[116] = 'a';
    strcpy(longname + 117, ".otf");
    assert(!decode_name_limit(longname, name, sizeof(name), 2));
    assert(check_length(1048577, 1048576, UINT64_MAX) == 413);
    assert(check_length(17, 0, 16) == 507);
    assert(check_length(0, 0, 100) == 400);
    assert(check_length(1048576, 1048576, 1048576) == 0);
    snprintf(path, sizeof(path), "%s/book.txt", root);
    snprintf(part, sizeof(part), "%s.part", path);
    write_fixture(path, "original"); remaining = 50;
    transfer_book_page_t listing;
    assert(list_books(root, "BOOK", 0, &listing) == 0 && listing.total == 1 && listing.items[0].size == 8);
    assert(list_books(root, "missing", 0, &listing) == 0 && listing.total == 0);
    file_result_t op = upload_managed(path, part, NULL, 50, 0, UINT64_MAX, false, buf, sizeof(buf), receive, NULL, NULL, change_callback);
    assert(op.status == 409 && !op.changed && remaining == 50 && cleanup_calls == 0);
    fail_recv = 1;
    op = upload_managed(path, part, NULL, 50, 0, UINT64_MAX, true, buf, sizeof(buf), receive, NULL, NULL, change_callback);
    assert(op.status == 408 && !op.changed && cleanup_calls == 0); fail_recv = 0;
    cleanup_failure = 1; remaining = 50;
    op = upload_managed(path, part, NULL, 50, 0, UINT64_MAX, true, buf, sizeof(buf), receive, NULL, NULL, change_callback);
    assert(op.status == 500 && op.changed && op.progress_cleanup_failed && cleanup_calls == 1);
    op = delete_managed(path, change_callback);
    assert(op.status == 500 && op.changed && op.progress_cleanup_failed && access(path, F_OK) != 0);
    cleanup_failure = 0;
    assert(change_callback(path) == 0);
    op = delete_managed(path, change_callback); assert(op.status == 404 && !op.changed);
    remaining = 50;
    assert(receive_file(path, part, NULL, 50, buf, sizeof(buf), receive, NULL, NULL) == 0);
    FILE *f = fopen(path, "rb"); assert(f); fseek(f, 0, SEEK_END); assert(ftell(f) == 50); fclose(f);
    remaining = 20;
    assert(receive_file(path, part, NULL, 20, buf, sizeof(buf), receive, NULL, NULL) == 0);
    fail_recv = 1;
    assert(receive_file(path, part, NULL, 20, buf, sizeof(buf), receive, NULL, NULL) == 408);
    assert(access(part, F_OK) != 0);
    fail_recv = 0; remaining = 20; fail_write = 1;
    assert(receive_file(path, part, NULL, 20, buf, sizeof(buf), receive, NULL, NULL) == 507);
    assert(access(part, F_OK) != 0); fail_write = 0;
    remaining = 20; fail_close = 1;
    assert(receive_file(path, part, NULL, 20, buf, sizeof(buf), receive, NULL, NULL) == 507);
    assert(access(part, F_OK) != 0); fail_close = 0;
    fatfs_replace = 1; remaining = 30;
    assert(receive_file(path, part, NULL, 30, buf, sizeof(buf), receive, NULL, NULL) == 0);
    fail_commit = 1; remaining = 40;
    assert(receive_file(path, part, NULL, 40, buf, sizeof(buf), receive, NULL, NULL) == 507);
    f = fopen(path, "rb"); assert(f); fseek(f, 0, SEEK_END); assert(ftell(f) == 30); fclose(f);
    fail_commit = 0;
    char backup[320]; snprintf(backup, sizeof(backup), "%s.rename-backup", path);
    f = fopen(backup, "wb"); assert(f); fclose(f); remaining = 40;
    assert(receive_file(path, part, NULL, 40, buf, sizeof(buf), receive, NULL, NULL) == 507);
    assert(access(backup, F_OK) == 0); unlink(backup);
    f = fopen(path, "rb"); assert(f); fseek(f, 0, SEEK_END); assert(ftell(f) == 30); fclose(f);
    fail_recv = 0; remaining = 0;
    assert(receive_file(path, part, NULL, 20, buf, sizeof(buf), receive, NULL, NULL) == 408);
    remaining = 20;
    assert(receive_file(root, part, NULL, 20, buf, sizeof(buf), receive, NULL, NULL) == 507);
    assert(access(part, F_OK) != 0);
    assert(receive_file(path, "/nonexistent/part", NULL, 20, buf, sizeof(buf), receive, NULL, NULL) == 507);
    unlink(path);
    char orphan[320], saved[320], kept[320], unknown[320], directory[320];
    snprintf(orphan, sizeof(orphan), "%s/orphan.epub.part", root);
    snprintf(saved, sizeof(saved), "%s/orphan.epub.rename-backup", root);
    snprintf(kept, sizeof(kept), "%s/orphan.epub", root);
    snprintf(unknown, sizeof(unknown), "%s/notes.pdf.part", root);
    snprintf(directory, sizeof(directory), "%s/directory.txt.part", root);
    write_fixture(orphan, "incomplete"); write_fixture(saved, "old complete book");
    write_fixture(unknown, "not ours"); assert(mkdir(directory, 0700) == 0);
    unsigned removed = 0, restored = 0;
    assert(cleanup_interrupted(root, TRANSFER_MEDIA_NAME_MAX, &removed, &restored) == 0 && removed == 1 && restored == 1);
    assert(access(orphan, F_OK) != 0 && access(saved, F_OK) != 0);
    f = fopen(kept, "rb"); assert(f); char content[32] = {0}; assert(fread(content, 1, 31, f) == 17); fclose(f);
    assert(!strcmp(content, "old complete book"));
    assert(access(unknown, F_OK) == 0 && access(directory, F_OK) == 0);
    write_fixture(saved, "second complete book");
    assert(cleanup_interrupted(root, TRANSFER_MEDIA_NAME_MAX, &removed, &restored) == 0 && removed == 0 && restored == 0);
    assert(access(saved, F_OK) == 0 && access(kept, F_OK) == 0);
    char percent[320], invalid[320];
    snprintf(percent, sizeof(percent), "%s/100%%.txt.part", root);
    snprintf(invalid, sizeof(invalid), "%s/bad..txt.part", root);
    write_fixture(percent, "incomplete"); write_fixture(invalid, "not ours");
    fail_remove = 1;
    assert(cleanup_interrupted(root, TRANSFER_MEDIA_NAME_MAX, &removed, &restored) != 0 && access(percent, F_OK) == 0);
    fail_remove = 0;
    assert(cleanup_interrupted(root, TRANSFER_MEDIA_NAME_MAX, &removed, &restored) == 0 && removed == 1);
    assert(access(percent, F_OK) != 0 && access(invalid, F_OK) == 0);
    assert(cleanup_interrupted("/nonexistent/cleanup", TRANSFER_MEDIA_NAME_MAX, &removed, &restored) != 0);
    unlink(saved); unlink(kept); unlink(unknown); unlink(invalid); rmdir(directory);
    char long_name[256], managed_path[448], decoded[256];
    memset(long_name, 'x', 251); strcpy(long_name + 251, ".txt");
    assert(raw_book_name(long_name, decoded, sizeof(decoded)) && strlen(decoded) == 255);
    assert(!decode_name_limit(long_name, name, sizeof(name), 0));
    snprintf(managed_path, sizeof(managed_path), "%s/%s", root, long_name); write_fixture(managed_path, "long");
    for (int i = 0; i < 65; ++i) { snprintf(path, sizeof(path), "%s/pg%02d.txt", root, i); write_fixture(path, "page"); }
    assert(list_books(root, "", 0, &listing) == 0 && listing.total == 66 && listing.count == 16);
    assert(list_books(root, "", 4, &listing) == 0 && listing.total == 66 && listing.count == 2);
    assert(list_books(root, "PG0", 0, &listing) == 0 && listing.total == 10);
    assert(delete_managed(managed_path, change_callback).changed);
    memset(long_name, 'x', 196); strcpy(long_name + 196, ".txt");
    snprintf(managed_path, sizeof(managed_path), "%s/%s", root, long_name);
    snprintf(backup, sizeof(backup), "%s/%s.rename-backup", root, long_name);
    write_fixture(managed_path, "long main"); write_fixture(backup, "long backup");
    assert(delete_managed(managed_path, change_callback).changed);
    assert(access(managed_path, F_OK) != 0 && access(backup, F_OK) != 0);
    assert(delete_managed(root, change_callback).status == 400);
    for (int i = 0; i < 65; ++i) { snprintf(path, sizeof(path), "%s/pg%02d.txt", root, i); unlink(path); }
    snprintf(path, sizeof(path), "%s/large.epub", root); snprintf(part, sizeof(part), "%s.part", path);
    char *large_buf = malloc(16384); assert(large_buf);
    remaining = 50 * 1024 * 1024;
    op = upload_managed(path, part, NULL, (size_t)remaining, 0, UINT64_MAX, false, large_buf, 16384, receive, NULL, NULL, change_callback);
    assert(op.status == 200 && op.changed && remaining == 0);
    fail_after = 3; receive_calls = 0; remaining = 1024 * 1024;
    op = upload_managed(path, part, NULL, (size_t)remaining, 0, UINT64_MAX, true, large_buf, 16384, receive, NULL, NULL, change_callback);
    assert(op.status == 408 && !op.changed && remaining < 1024 * 1024 && access(part, F_OK) != 0);
    struct stat large_stat; assert(stat(path, &large_stat) == 0 && large_stat.st_size == 50 * 1024 * 1024);
    free(large_buf); unlink(path);
    snprintf(path, sizeof(path), "%s/backup-warning.txt", root); snprintf(part, sizeof(part), "%s.part", path);
    snprintf(backup, sizeof(backup), "%s.rename-backup", path); write_fixture(path, "old");
    fail_after = 0; fail_remove = 1; remaining = 16; int before_callback = cleanup_calls;
    op = upload_managed(path, part, NULL, 16, 0, UINT64_MAX, true, buf, sizeof(buf), receive, NULL, NULL, change_callback);
    assert(op.status == 200 && op.changed && cleanup_calls == before_callback + 1);
    assert(op.storage_cleanup_failed);
    assert(stat(path, &large_stat) == 0 && large_stat.st_size == 16 && access(backup, F_OK) == 0);
    fail_remove = 0; unlink(path); unlink(backup);
    write_fixture(path, "new complete"); write_fixture(backup, "old complete");
    op = delete_managed(path, change_callback);
    assert(op.status == 200 && op.changed && access(path, F_OK) != 0 && access(backup, F_OK) != 0);
    assert(cleanup_interrupted(root, TRANSFER_MEDIA_NAME_MAX, &removed, &restored) == 0 && restored == 0);
    write_fixture(path, "main stays"); write_fixture(backup, "old stays"); fail_remove = 1;
    op = delete_managed(path, change_callback);
    assert(op.status == 507 && !op.changed && access(path, F_OK) == 0 && access(backup, F_OK) == 0);
    fail_remove = 0; unlink(path); fail_restore = 1;
    op = delete_managed(path, change_callback);
    assert(op.status == 507 && !op.changed && access(path, F_OK) != 0 && access(backup, F_OK) == 0);
    fail_restore = 0; fail_remove = 1;
    op = delete_managed(path, change_callback);
    assert(op.status == 507 && !op.changed && access(path, F_OK) == 0 && access(backup, F_OK) != 0);
    fail_remove = 0; unlink(path); write_fixture(backup, "recover then delete");
    op = delete_managed(path, change_callback);
    assert(op.status == 200 && op.changed && access(path, F_OK) != 0 && access(backup, F_OK) != 0);
    unsigned changes = 0;
    record_file_change(&changes, (file_result_t){.status = 200}, true); assert(changes == 1);
    record_file_change(&changes, (file_result_t){.status = 500}, true); assert(changes == 1);
    record_file_change(&changes, (file_result_t){.status = 500, .changed = true}, false); assert(changes == 2);
    // 别名覆盖和删除都必须清真实路径，覆盖不改变目录拼写。/ Alias replacement and deletion clear the actual path and preserve directory spelling.
    snprintf(path, sizeof(path), "%s/Book.txt", root); write_fixture(path, "old data");
    assert(resolve_mutation_path(root, "book.txt", requested, sizeof(requested)) == 0);
    snprintf(part, sizeof(part), "%s.part", path); remaining = 8; cleanup_failure = 0;
    op = upload_managed(requested, part, NULL, 8, 0, UINT64_MAX, true, buf, sizeof(buf), receive, NULL, NULL, change_callback);
    assert(op.status == 200 && op.changed && !strcmp(cleaned_path, path));
    assert(access(path, F_OK) == 0);
    assert(resolve_mutation_path(root, "BOOK.TXT", requested, sizeof(requested)) == 0);
    cleanup_failure = 1;
    op = delete_managed(requested, change_callback);
    assert(op.status == 500 && op.changed && op.progress_cleanup_failed && !strcmp(cleaned_path, path));
    assert(access(path, F_OK) != 0);
    cleanup_failure = 0;
    assert(resolve_mutation_path(root, "Book.txt", requested, sizeof(requested)) == 0);
    op = changed_result(requested, change_callback);
    assert(op.status == 200 && !strcmp(cleaned_path, path));
    rmdir(root); puts("transfer host tests passed");
}
