#pragma once
// Minimal host port for exercising the production GIF decoder under sanitizers.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
#define LV_GIF_CACHE_DECODE_DATA 0
#define LV_USE_DRAW_SW_ASM 0
#define LV_DRAW_SW_ASM_HELIUM 1
#define LV_FS_MODE_RD 0
#define LV_FS_SEEK_SET 0
#define LV_FS_SEEK_CUR 1
#define LV_FS_RES_OK 0
#define lv_malloc malloc
#define lv_realloc realloc
#define lv_free free
typedef struct { int unused; } lv_fs_file_t;
typedef int lv_fs_res_t;
static inline int lv_fs_open(lv_fs_file_t* f, const void* p, int m) { (void)f; (void)p; (void)m; return -1; }
static inline int lv_fs_read(lv_fs_file_t* f, void* p, size_t n, uint32_t* r) { (void)f; (void)p; (void)n; (void)r; return -1; }
static inline int lv_fs_seek(lv_fs_file_t* f, size_t p, int k) { (void)f; (void)p; (void)k; return -1; }
static inline int lv_fs_tell(lv_fs_file_t* f, uint32_t* p) { (void)f; *p = 0; return -1; }
static inline int lv_fs_close(lv_fs_file_t* f) { (void)f; return 0; }
