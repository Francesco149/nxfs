/*
nxfs - read-only fuse file system for maplestory .nx files
based on tinynx (https://github.com/Francesco149/tinynx)
-------------------------------------------------------------------
this is free and unencumbered software released into the
public domain.

refer to the attached UNLICENSE or http://unlicense.org/

credits to retep998, angelsl and everyone else who partecipated
in writing the nx format specification: http://nxformat.github.io/

LZ4 - Fast LZ compression algorithm
is Copyright (C) 2011-2017, Yann Collet.
*/

#if __STDC_VERSION__ >= 199901L
#define _XOPEN_SOURCE 600
#else
#define _XOPEN_SOURCE 500
#endif

#define _REENTRANT 1

#define FUSE_USE_VERSION 29

#define NX_IMPLEMENTATION
#define NX_MONOLITHIC
#include "nx.c"

#define NXFS_VERSION_MAJOR 1
#define NXFS_VERSION_MINOR 0
#define NXFS_VERSION_PATCH 0

#include <fuse.h>
#include <fuse_opt.h>
#ifndef __CYGWIN__
#   include <fuse_lowlevel.h>
#endif
#ifdef __APPLE__
#   include <fuse_darwin.h>
#endif

#include <errno.h>
#include <inttypes.h>

#define internalfn static
#define global static

global int multithreaded = 1;
global char const* nxpath = 0;
global char const* mountpoint = 0;
global struct nx_file nx;
global struct stat nxstat;
global struct fuse_operations ops;

#define mymax(a, b) ((a) > (b) ? (a) : (b))
#define has_suffix(str, suffix) \
    (!strcmp(mymax((str), (str) + strlen(str) - strlen(suffix)), \
        (suffix)))

/* hack to display type in the filename */
internalfn
char* trim_path(char const* path)
{
    char* res = strdup(path);

    if (has_suffix(path, ".int64") || has_suffix(path, ".real") ||
        has_suffix(path, ".string") || has_suffix(path, "vector")||
        has_suffix(path, ".vector") || has_suffix(path, ".bmp") ||
        has_suffix(path, ".mp3"))
    {
        char* p = res + strlen(res) - 1;
        for (; *p != '.'; *p-- = 0);
        *p-- = 0;
    }

    return res;
}

internalfn
void json_integer(char* dst, uint16_t node_type,
    uint8_t const* node_data)
{
    switch (node_type)
    {
    case NX_INT64:
        sprintf(dst, "%" PRId64 "\n", (int64_t)read8(node_data));
        break;

    case NX_REAL:
        sprintf(dst, "%.17g\n", read_double(node_data));
        break;

    case NX_VECTOR:
        sprintf(dst, "[%d,%d]\n", (int32_t)read4(node_data),
            (int32_t)read4(node_data + 4));
        break;

    default:
        *dst = 0;
    }
}

internalfn
void json_string(char* dst, char* src)
{
    char const* const chars_to_escape = "\\\"";
    char const* p;

    *dst++ = '"';

    for (; *src; ++src)
    {
        /* escape all characters in chars_to_escape */
        for (p = chars_to_escape; *p; ++p) {
            if (*p == *src) {
                *dst++ = '\\';
            }
        }

        *dst++ = *src;
    }

    *dst++ = '"';
    *dst++ = '\n';
    *dst++ = 0;
}

internalfn
int nxfs_getattr(char const* path, struct stat* st)
{
    int error = 0;
    int32_t res;
    struct nx_node node;
    char* trimmed_path = trim_path(path);

    st->st_uid = nxstat.st_uid;
    st->st_gid = nxstat.st_gid;
    st->st_atime = nxstat.st_atime;
    st->st_mtime = nxstat.st_mtime;

    res = nx_get(&nx, trimmed_path, &node);
    free(trimmed_path);
    if (res < 0) {
        info("nx_get: %s\n", nx_errstr(res));
        error = -ENOENT;
        goto cleanup;
    }

    if (node.type == NX_NONE) {
        /* directory */
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
    } else {
        /* file */
        st->st_mode = S_IFREG | 0644;
        st->st_nlink = 1;
    }

    switch (node.type)
    {
    case NX_STRING:
    {
        /* TODO: is this too much for the stack? */
        char buf[0x10000];
        char jsonbuf[0x20002];

        /* TODO: implement nx_strlen_at for better performance */
        res = nx_string_at(&nx, read4(node.data), buf);
        if (res < 0) {
            info("nx_string_at: %s\n", nx_errstr(res));
            error = -ENOENT;
            goto cleanup;
        }

        json_string(jsonbuf, buf);
        st->st_size = strlen(jsonbuf);
        break;
    }

#ifndef NX_NOBITMAP
    case NX_BITMAP:
        st->st_size = 14 + 40 + 4 * 4 + /* headers */
            read2(node.data + 4) * /* width */
            read2(node.data + 6) * /* height */
            4; /* 4 bytes per pixel */
        break;
#endif

    case NX_AUDIO:
        st->st_size = read4(node.data + 4);
        break;

    case NX_INT64:
    case NX_REAL:
    case NX_VECTOR:
    {
        char buf[0x10000];
        json_integer(buf, node.type, node.data);
        st->st_size = strlen(buf);
        break;
    }

    default:
        st->st_size = 8;
        break;
    }

cleanup:
    return error;
}

internalfn
char const* extension(uint16_t type)
{
    switch (type)
    {
    case NX_INT64: return ".int64";
    case NX_REAL: return ".real";
    case NX_STRING: return ".string";
    case NX_VECTOR: return ".vector";
#ifndef NX_NOBITMAP
    case NX_BITMAP: return ".bmp";
#endif
    case NX_AUDIO: return ".mp3";
    }

    return "";
}

internalfn
int nxfs_readdir(char const* path, void* buffer,
    fuse_fill_dir_t filler, off_t offset,
    struct fuse_file_info* fi)
{
    char buf[0x10000];

    int error = 0;
    int32_t res;
    uint32_t i, nchildren, first_child;
    struct nx_node node;
    char* trimmed_path = trim_path(path);

    filler(buffer, ".", 0, 0);
    filler(buffer, "..", 0, 0);

    res = nx_get(&nx, trimmed_path, &node);
    free(trimmed_path);
    if (res < 0)
    {
        info("nx_get: %s\n", nx_errstr(res));
        error = -ENOENT;
        goto cleanup;
    }

    nchildren = node.nchildren;
    first_child = node.first_child_id;

    for (i = 0; i < nchildren; ++i)
    {
        res = nx_node_at(&nx, first_child + i, &node);
        if (res < 0) {
            info("nx_node_at: %s\n", nx_errstr(res));
            continue;
        }

        res = nx_string_at(&nx, node.name_id, buf);
        if (res < 0) {
            info("nx_string_at: %s\n", nx_errstr(res));
            continue;
        }

        strcat(buf, extension(node.type));
        filler(buffer, buf, 0, 0);
    }

cleanup:
    return error;
}

internalfn
void copy2(uint8_t* dst, uint16_t v)
{
    dst[0] = (uint8_t)(v & 0xFF);
    dst[1] = (uint8_t)(v >> 8);
}

internalfn
void copy4(uint8_t* dst, uint32_t v)
{
    dst[0] = (uint8_t)(v & 0xFF);
    dst[1] = (uint8_t)((v >> 8) & 0xFF);
    dst[2] = (uint8_t)((v >> 16) & 0xFF);
    dst[3] = (uint8_t)((v >> 24) & 0xFF);
}

internalfn
int nxfs_read(char const* path, char* dst, size_t size_,
    off_t offset, struct fuse_file_info* fi)
{
    int64_t size = (int64_t)(size_t)size_;
    int32_t res = 0;
    struct nx_node node;

    char* trimmed_path = trim_path(path);
    res = nx_get(&nx, trimmed_path, &node);
    free(trimmed_path);
    if (res < 0) {
        info("nx_get: %s\n", nx_errstr(res));
        res = -ENOENT;
        goto cleanup;
    }

    switch (node.type)
    {
    case NX_NONE: break;

    case NX_STRING:
    {
        char buf[0x10000];
        char jsonbuf[0x20002];

        res = nx_string_at(&nx, read4(node.data), buf);
        if (res < 0) {
            info("nx_string_at: %s\n", nx_errstr(res));
            res = 0;
            break;
        }

        json_string(jsonbuf, buf);

        res = mymin(strlen(jsonbuf) - offset, size);
        if (res > 0) {
            memcpy(dst, jsonbuf + offset, res);
        } else {
            res = 0;
        }

        break;
    }

#ifndef NX_NOBITMAP
    case NX_BITMAP:
    {
        uint16_t width = read2(node.data + 4);
        uint16_t height = read2(node.data + 6);
        uint8_t header[14 + 40 + 4 * 4];
        uint8_t* p;
        int32_t uncompressed_size = width * height * 4;
        uint8_t* pixels = 0;
        int32_t to_copy;

        /* bfType = BM (windblows) */
        p = header;
        memcpy(p, "BM", 2); p += 2;
        copy4(p, 14 + 40 + 4 * 4 + uncompressed_size); /* bfSize */
        p += 4;
        copy2(p, 0); p += 2;
        copy2(p, 0); p += 2;
        copy4(p, 14 + 40); p += 4; /* bfOffBits */

        copy4(p, 40); p += 4; /* biSize */
        copy4(p, width); p += 4;
        copy4(p, (uint32_t)(-(int32_t)height)); /* top-down */
        p += 4;
        copy2(p, 1); p += 2; /* biPlanes */
        copy2(p, 32); p += 2; /* biBitCount */
        copy4(p, 3); p += 4; /* biCompression =  BI_BITFIELDS */
        copy4(p, uncompressed_size); p += 4; /* biSizeImage */
        copy4(p, 2835); p += 4;/* biXPelsPerMeter */
        copy4(p, 2835); p += 4; /* biYPelsPerMeter */
        copy4(p, 0); p += 4; /* biClrUsed */
        copy4(p, 0); p += 4; /* biClrImportant */

        /* BGRA8888 -> RGBA8888 color masks */
        memcpy(
            p,
            "\x00\x00\xFF\x00"
            "\x00\xFF\x00\x00"
            "\xFF\x00\x00\x00"
            "\x00\x00\x00\xFF",
            4 * 4
        );

        p += 4 * 4;

        if (offset < sizeof(header))
        {
            to_copy = mymin(size, sizeof(header) - offset);
            if (to_copy > 0)
            {
                memcpy(dst, header + offset, to_copy);
                size -= to_copy;
                offset = 0;
                dst += to_copy;
                res += to_copy;
            }
        }

        else {
            offset -= sizeof(header);
        }

        /* TODO: allow partial reads of the bitmap to save mem */
        if (size > 0)
        {
            int32_t result;

            to_copy = mymin(size, uncompressed_size - offset);

            pixels = (uint8_t*)malloc(uncompressed_size);
            result = nx_bitmap_at(&nx, read4(node.data), pixels,
                uncompressed_size);
            if (result < 0) {
                res = result;
                goto bmp_cleanup;
            }

            if (to_copy > 0) {
                memcpy(dst, pixels + offset, to_copy);
                res += to_copy;
            }
        }

bmp_cleanup:
        if (pixels) {
            free(pixels);
        }

        if (res < 0) {
            res = -ENOENT;
        }

        break;
    }
#endif

    case NX_AUDIO:
    {
        int32_t error;
        uint8_t const* raw_audio =
            nx_audio_at(&nx, read4(node.data), &error);
        if (!raw_audio || error < 0)
        {
            info("nx_audio_at failed\n");
            if (error < 0) {
                info("%s\n", nx_errstr(error));
            }
        }

        res = mymin(read4(node.data + 4) - offset, size);
        if (res > 0) {
            memcpy(dst, raw_audio + offset, res);
        } else {
            res = 0;
        }

        break;
    }

    case NX_INT64:
    case NX_REAL:
    case NX_VECTOR:
    {
        char buf[0x10000];
        json_integer(buf, node.type, node.data);

        res = mymin(strlen(buf) - offset, size);
        if (res > 0) {
            memcpy(dst, buf + offset, res);
        } else {
            res = 0;
        }
        break;
    }

    default:
        res = mymin(8 - offset, size);
        if (res > 0) {
            memcpy(dst, node.data + offset, res);
        } else {
            res = 0;
        }
        break;
    }

cleanup:
    return res;
}

#define KEY_HELP 0
#define KEY_VERSION 1
#define KEY_SINGLE_THREADED 2

global struct fuse_opt nxfs_opts[] =
{
    FUSE_OPT_KEY("-v", KEY_VERSION),
    FUSE_OPT_KEY("--version", KEY_VERSION),
    FUSE_OPT_KEY("-h", KEY_HELP),
    FUSE_OPT_KEY("--help", KEY_HELP),
    FUSE_OPT_KEY("-s", KEY_SINGLE_THREADED),
    FUSE_OPT_END
};

internalfn
int nxfs_opt_proc(void* data, char const* arg, int key,
    struct fuse_args* args)
{
    switch (key)
    {
    case KEY_VERSION:
        printf("nxfs %d.%d.%d\n", NXFS_VERSION_MAJOR,
            NXFS_VERSION_MINOR, NXFS_VERSION_PATCH);
        printf("tinynx %d.%d.%d\n", NX_VERSION_MAJOR,
            NX_VERSION_MINOR, NX_VERSION_PATCH);

        fuse_opt_add_arg(args, "--version");
        fuse_main(args->argc, args->argv, &ops, 0);

        exit(0);

    case KEY_HELP:
        info(
            "usage: %s /path/to/file.nx mountpoint [options]\n"
            "\n"
            "-v  --version print version information\n"
            "-h  --help    print this help\n"
            "-ho           show fuse otions\n\n",
            args->argv[0]
        );

        fuse_main(args->argc, args->argv, &ops, 0);

        exit(1);

    case KEY_SINGLE_THREADED:
        multithreaded = 0;
        return 0;

    case FUSE_OPT_KEY_NONOPT:
        if (!nxpath) {
            nxpath = strdup(arg);
            return 0;
        }

        else if (!mountpoint) {
            mountpoint = strdup(arg);
            return 0;
        }

        info("nxfs: invalid argument '%s'\n", arg);
        return 1;
    }

    return 1;
}

int main(int argc, char* argv[])
{
    struct fuse* fuse = 0;
    struct fuse_chan* ch = 0;

    int32_t res = 0;
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    memset(&ops, 0, sizeof(ops));
    ops.getattr = nxfs_getattr;
    ops.readdir = nxfs_readdir;
    ops.read = nxfs_read;

    fuse_opt_parse(&args, &nxpath, nxfs_opts, nxfs_opt_proc);

    if (!nxpath || !mountpoint) {
        fuse_opt_add_arg(&args, "-h");
        fuse_opt_parse(&args, &nxpath, nxfs_opts, nxfs_opt_proc);
        goto runmain;
    }

    if (stat(nxpath, &nxstat) < 0) {
        perror("stat");
        res = NX_EIO;
        goto runmain;
    }

    res = nx_map(&nx, nxpath);

runmain:
    if (res < 0) {
        info("%s\n", nx_errstr(res));
        res = 1; goto cleanup;
    }

    /* low level main so i can override args and provide the nx
    file with no option prefix. mostly copied from sshfs */

    ch = fuse_mount(mountpoint, &args);
    if (!ch) {
        res = 1; goto cleanup;
    }

#if !defined(__CYGWIN__)
    res = fcntl(fuse_chan_fd(ch), F_SETFD, FD_CLOEXEC);
    if (res < 0) {
        perror("WARNING: failed to set FD_CLOEXEC on fuse device");
    }
#endif

    fuse = fuse_new(ch, &args, &ops, sizeof(ops), 0);
    if (!fuse) {
        res = 1; goto cleanup;
    }

    res = fuse_daemonize(0);
    if (res < 0) {
        res = 1; goto cleanup;
    }

    res = fuse_set_signal_handlers(fuse_get_session(fuse));
    if (res) {
        res = 1; goto cleanup;
    }

    res = (multithreaded ? fuse_loop_mt : fuse_loop)(fuse) != 0;

cleanup:
    if (ch) {
        fuse_unmount(mountpoint, ch);
    }

    if (fuse)
    {
        if (fuse_get_session(fuse)) {
            fuse_remove_signal_handlers(fuse_get_session(fuse));
        }

        fuse_destroy(fuse);
    }

    fuse_opt_free_args(&args);

    return res;
}

