/*
 * TBCAD plugin API, version 1. Plain C so a plugin can be written in C, C++, Rust, Zig, ...
 *
 * A plugin is a shared library (.dll / .so / .dylib) in the "plugins" folder next to the
 * settings. It exports
 *
 *     int tbcad_plugin_init(const TbcadHost* host);
 *
 * which TBCAD calls once at start-up. The plugin registers its commands there:
 *
 *     host->register_command("HELLO", "Draws a line", hello_command, NULL);
 *
 * A command is an ordinary blocking C function. It asks the user for input with the get_* calls
 * and changes the drawing with the add_* / erase / move calls; TBCAD runs it on its own thread
 * and hands control back and forth, so the user interface stays responsive while it waits for a
 * point and the whole command is one undo step. Every call that takes `ctx` must be made from
 * inside the command function with the ctx it was given.
 *
 * This header is licensed under the MIT licence like the rest of TBCAD, so a plugin does not
 * have to open its source.
 */
#ifndef TBCAD_PLUGIN_H
#define TBCAD_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TBCAD_PLUGIN_ABI_VERSION 1

#if defined(_WIN32)
#define TBCAD_PLUGIN_EXPORT __declspec(dllexport)
#else
#define TBCAD_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

/* Result of every call that can fail or be cancelled. */
enum {
    TBCAD_OK = 0,      /* the call worked / the user answered */
    TBCAD_CANCEL = 1,  /* the user pressed Esc: return from the command now */
    TBCAD_NONE = 2,    /* the user pressed Enter without answering */
    TBCAD_KEYWORD = 3, /* the user chose a keyword (see the `keyword` argument) */
    TBCAD_ERROR = 4    /* the request was wrong (bad id, zero radius, ...) */
};

/* Entity kinds reported by get_entity. */
enum {
    TBCAD_ENTITY_OTHER = 0,
    TBCAD_ENTITY_LINE = 1,
    TBCAD_ENTITY_CIRCLE = 2,
    TBCAD_ENTITY_ARC = 3,
    TBCAD_ENTITY_POLYLINE = 4,
    TBCAD_ENTITY_POINT = 5,
    TBCAD_ENTITY_ELLIPSE = 6,
    TBCAD_ENTITY_SPLINE = 7,
    TBCAD_ENTITY_TEXT = 8,
    TBCAD_ENTITY_MTEXT = 9,
    TBCAD_ENTITY_HATCH = 10,
    TBCAD_ENTITY_INSERT = 11,
    TBCAD_ENTITY_DIMENSION = 12,
    TBCAD_ENTITY_LEADER = 13
};

typedef struct TbcadCommandCtx TbcadCommandCtx;

/* The command function. Return 0 when done (also after TBCAD_CANCEL). */
typedef int (*TbcadCommandFn)(TbcadCommandCtx* ctx, void* user);

typedef struct TbcadEntityInfo {
    int kind;                          /* TBCAD_ENTITY_* */
    double min_x, min_y, max_x, max_y; /* bounding box in drawing units */
    char layer[64];                    /* layer name, UTF-8, truncated */
} TbcadEntityInfo;

typedef struct TbcadHost {
    uint32_t abi_version; /* TBCAD_PLUGIN_ABI_VERSION of the running program */
    uint32_t size;        /* sizeof(TbcadHost); fields are only ever added at the end */

    /* ---- start-up (call from tbcad_plugin_init) ------------------------------------------ */
    /* Adds a command. `name` is upper-case ASCII; it must not clash with an existing command.
       Returns 0 on success, non-zero if the name is taken or invalid. */
    int (*register_command)(const char* name, const char* description, TbcadCommandFn fn,
                            void* user);
    /* Adds a menu entry, for example ("Plugins/Hello", "HELLO"). */
    void (*add_menu_item)(const char* path, const char* command);
    /* Writes a line to the message log (usable any time). */
    void (*log)(const char* message);

    /* ---- inside a command ---------------------------------------------------------------- */
    void (*message)(TbcadCommandCtx* ctx, const char* text);

    /* Prompts. `prompt` is shown on the command line. */
    int (*get_point)(TbcadCommandCtx* ctx, const char* prompt, const double* base_xy_or_null,
                     double* x, double* y);
    int (*get_real)(TbcadCommandCtx* ctx, const char* prompt, double* value);
    int (*get_integer)(TbcadCommandCtx* ctx, const char* prompt, int64_t* value);
    int (*get_string)(TbcadCommandCtx* ctx, const char* prompt, char* out, size_t capacity);
    /* `keywords` is a '/'-separated list such as "Yes/No". Returns TBCAD_KEYWORD and the chosen
       one in `out`. */
    int (*get_keyword)(TbcadCommandCtx* ctx, const char* prompt, const char* keywords, char* out,
                       size_t capacity);
    /* Objects: the current selection if there is one, otherwise the user selects. Writes up to
       `capacity` ids and the total number to `count`. */
    int (*get_selection)(TbcadCommandCtx* ctx, const char* prompt, uint64_t* ids, size_t capacity,
                         size_t* count);

    /* Drawing: new entities go on the current layer. `id_out` may be NULL. */
    int (*add_line)(TbcadCommandCtx* ctx, double x1, double y1, double x2, double y2,
                    uint64_t* id_out);
    int (*add_circle)(TbcadCommandCtx* ctx, double cx, double cy, double radius, uint64_t* id_out);
    /* `xy` holds `count` x,y pairs. */
    int (*add_polyline)(TbcadCommandCtx* ctx, const double* xy, size_t count, int closed,
                        uint64_t* id_out);
    int (*erase)(TbcadCommandCtx* ctx, uint64_t id);
    int (*move)(TbcadCommandCtx* ctx, uint64_t id, double dx, double dy);
    int (*get_entity)(TbcadCommandCtx* ctx, uint64_t id, TbcadEntityInfo* info);
    /* Ids of every visible model-space entity: up to `capacity` of them, total in `count`. */
    int (*list_entities)(TbcadCommandCtx* ctx, uint64_t* ids, size_t capacity, size_t* count);
} TbcadHost;

/* Optional: describes the plugin for the Plugins list. */
typedef struct TbcadPluginInfo {
    uint32_t abi_version; /* TBCAD_PLUGIN_ABI_VERSION the plugin was built against */
    const char* name;
    const char* version;
    const char* author;
} TbcadPluginInfo;

/* Exported by the plugin: */
/* TBCAD_PLUGIN_EXPORT int tbcad_plugin_init(const TbcadHost* host);            required */
/* TBCAD_PLUGIN_EXPORT const TbcadPluginInfo* tbcad_plugin_info(void);          optional */

#ifdef __cplusplus
}
#endif

#endif /* TBCAD_PLUGIN_H */
