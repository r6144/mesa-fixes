/*
 * Copyright 2008 Corbin Simpson <MostAwesomeDude@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE. */

#ifndef R300_CONTEXT_H
#define R300_CONTEXT_H

#include "draw/draw_vertex.h"

#include "util/u_blitter.h"

#include "pipe/p_context.h"
#include "util/u_inlines.h"
#include "util/u_transfer.h"

#include "r300_defines.h"
#include "r300_screen.h"

struct u_upload_mgr;
struct r300_context;

struct r300_fragment_shader;
struct r300_vertex_shader;

struct r300_atom {
    /* List pointers. */
    struct r300_atom *prev, *next;
    /* Name, for debugging. */
    const char* name;
    /* Stat counter. */
    uint64_t counter;
    /* Opaque state. */
    void* state;
    /* Emit the state to the context. */
    void (*emit)(struct r300_context*, unsigned, void*);
    /* Upper bound on number of dwords to emit. */
    unsigned size;
    /* Whether this atom should be emitted. */
    boolean dirty;
    /* Whether this atom may be emitted with state == NULL. */
    boolean allow_null_state;
};

struct r300_blend_state {
    uint32_t blend_control;       /* R300_RB3D_CBLEND: 0x4e04 */
    uint32_t alpha_blend_control; /* R300_RB3D_ABLEND: 0x4e08 */
    uint32_t color_channel_mask;  /* R300_RB3D_COLOR_CHANNEL_MASK: 0x4e0c */
    uint32_t rop;                 /* R300_RB3D_ROPCNTL: 0x4e18 */
    uint32_t dither;              /* R300_RB3D_DITHER_CTL: 0x4e50 */
};

struct r300_blend_color_state {
    /* RV515 and earlier */
    uint32_t blend_color;            /* R300_RB3D_BLEND_COLOR: 0x4e10 */
    /* R520 and newer */
    uint32_t blend_color_red_alpha;  /* R500_RB3D_CONSTANT_COLOR_AR: 0x4ef8 */
    uint32_t blend_color_green_blue; /* R500_RB3D_CONSTANT_COLOR_GB: 0x4efc */
};

struct r300_dsa_state {
    uint32_t alpha_function;    /* R300_FG_ALPHA_FUNC: 0x4bd4 */
    uint32_t alpha_reference;   /* R500_FG_ALPHA_VALUE: 0x4be0 */
    uint32_t z_buffer_control;  /* R300_ZB_CNTL: 0x4f00 */
    uint32_t z_stencil_control; /* R300_ZB_ZSTENCILCNTL: 0x4f04 */
    uint32_t stencil_ref_mask;  /* R300_ZB_STENCILREFMASK: 0x4f08 */
    uint32_t stencil_ref_bf;    /* R500_ZB_STENCILREFMASK_BF: 0x4fd4 */

    /* Whether a two-sided stencil is enabled. */
    boolean two_sided;
    /* Whether a fallback should be used for a two-sided stencil ref value. */
    boolean stencil_ref_bf_fallback;
};

struct r300_rs_state {
    /* Original rasterizer state. */
    struct pipe_rasterizer_state rs;
    /* Draw-specific rasterizer state. */
    struct pipe_rasterizer_state rs_draw;

    uint32_t vap_control_status;    /* R300_VAP_CNTL_STATUS: 0x2140 */
    uint32_t antialiasing_config;   /* R300_GB_AA_CONFIG: 0x4020 */
    uint32_t point_size;            /* R300_GA_POINT_SIZE: 0x421c */
    uint32_t point_minmax;          /* R300_GA_POINT_MINMAX: 0x4230 */
    uint32_t line_control;          /* R300_GA_LINE_CNTL: 0x4234 */
    float depth_scale;            /* R300_SU_POLY_OFFSET_FRONT_SCALE: 0x42a4 */
                                  /* R300_SU_POLY_OFFSET_BACK_SCALE: 0x42ac */
    float depth_offset;           /* R300_SU_POLY_OFFSET_FRONT_OFFSET: 0x42a8 */
                                  /* R300_SU_POLY_OFFSET_BACK_OFFSET: 0x42b0 */
    uint32_t polygon_offset_enable; /* R300_SU_POLY_OFFSET_ENABLE: 0x42b4 */
    uint32_t cull_mode;             /* R300_SU_CULL_MODE: 0x42b8 */
    uint32_t line_stipple_config;   /* R300_GA_LINE_STIPPLE_CONFIG: 0x4328 */
    uint32_t line_stipple_value;    /* R300_GA_LINE_STIPPLE_VALUE: 0x4260 */
    uint32_t color_control;         /* R300_GA_COLOR_CONTROL: 0x4278 */
    uint32_t polygon_mode;          /* R300_GA_POLY_MODE: 0x4288 */
    uint32_t clip_rule;             /* R300_SC_CLIP_RULE: 0x43D0 */

    /* Specifies top of Raster pipe specific enable controls,
     * i.e. texture coordinates stuffing for points, lines, triangles */
    uint32_t stuffing_enable;       /* R300_GB_ENABLE: 0x4008 */

    /* Point sprites texture coordinates, 0: lower left, 1: upper right */
    float point_texcoord_left;      /* R300_GA_POINT_S0: 0x4200 */
    float point_texcoord_bottom;    /* R300_GA_POINT_T0: 0x4204 */
    float point_texcoord_right;     /* R300_GA_POINT_S1: 0x4208 */
    float point_texcoord_top;       /* R300_GA_POINT_T1: 0x420c */
};

struct r300_rs_block {
    uint32_t vap_vtx_state_cntl;  /* R300_VAP_VTX_STATE_CNTL: 0x2180 */
    uint32_t vap_vsm_vtx_assm;    /* R300_VAP_VSM_VTX_ASSM: 0x2184 */
    uint32_t vap_out_vtx_fmt[2];  /* R300_VAP_OUTPUT_VTX_FMT_[0-1]: 0x2090 */

    uint32_t ip[8]; /* R300_RS_IP_[0-7], R500_RS_IP_[0-7] */
    uint32_t count; /* R300_RS_COUNT */
    uint32_t inst_count; /* R300_RS_INST_COUNT */
    uint32_t inst[8]; /* R300_RS_INST_[0-7] */
};

struct r300_sampler_state {
    struct pipe_sampler_state state;

    uint32_t filter0;      /* R300_TX_FILTER0: 0x4400 */
    uint32_t filter1;      /* R300_TX_FILTER1: 0x4440 */
    uint32_t border_color; /* R300_TX_BORDER_COLOR: 0x45c0 */

    /* Min/max LOD must be clamped to [0, last_level], thus
     * it's dependent on a currently bound texture */
    unsigned min_lod, max_lod;
};

struct r300_texture_format_state {
    uint32_t format0; /* R300_TX_FORMAT0: 0x4480 */
    uint32_t format1; /* R300_TX_FORMAT1: 0x44c0 */
    uint32_t format2; /* R300_TX_FORMAT2: 0x4500 */
    uint32_t tile_config; /* R300_TX_OFFSET (subset thereof) */
};

struct r300_sampler_view {
    struct pipe_sampler_view base;

    /* Swizzles in the UTIL_FORMAT_SWIZZLE_* representation,
     * derived from base. */
    unsigned char swizzle[4];

    /* Copy of r300_texture::texture_format_state with format-specific bits
     * added. */
    struct r300_texture_format_state format;
};

struct r300_texture_fb_state {
    /* Colorbuffer. */
    uint32_t colorpitch[R300_MAX_TEXTURE_LEVELS]; /* R300_RB3D_COLORPITCH[0-3]*/
    uint32_t us_out_fmt; /* R300_US_OUT_FMT[0-3] */

    /* Zbuffer. */
    uint32_t depthpitch[R300_MAX_TEXTURE_LEVELS]; /* R300_RB3D_DEPTHPITCH */
    uint32_t zb_format; /* R300_ZB_FORMAT */
};

struct r300_texture_sampler_state {
    struct r300_texture_format_state format;
    uint32_t filter0;      /* R300_TX_FILTER0: 0x4400 */
    uint32_t filter1;      /* R300_TX_FILTER1: 0x4440 */
    uint32_t border_color;  /* R300_TX_BORDER_COLOR: 0x45c0 */
};

struct r300_textures_state {
    /* Textures. */
    struct r300_sampler_view *sampler_views[16];
    int sampler_view_count;
    /* Sampler states. */
    struct r300_sampler_state *sampler_states[16];
    int sampler_state_count;

    /* This is the merge of the texture and sampler states. */
    unsigned count;
    uint32_t tx_enable;         /* R300_TX_ENABLE: 0x4101 */
    struct r300_texture_sampler_state regs[16];
};

struct r300_vertex_stream_state {
    /* R300_VAP_PROG_STREAK_CNTL_[0-7] */
    uint32_t vap_prog_stream_cntl[8];
    /* R300_VAP_PROG_STREAK_CNTL_EXT_[0-7] */
    uint32_t vap_prog_stream_cntl_ext[8];

    unsigned count;
};

struct r300_viewport_state {
    float xscale;         /* R300_VAP_VPORT_XSCALE:  0x2098 */
    float xoffset;        /* R300_VAP_VPORT_XOFFSET: 0x209c */
    float yscale;         /* R300_VAP_VPORT_YSCALE:  0x20a0 */
    float yoffset;        /* R300_VAP_VPORT_YOFFSET: 0x20a4 */
    float zscale;         /* R300_VAP_VPORT_ZSCALE:  0x20a8 */
    float zoffset;        /* R300_VAP_VPORT_ZOFFSET: 0x20ac */
    uint32_t vte_control; /* R300_VAP_VTE_CNTL:      0x20b0 */
};

struct r300_ztop_state {
    uint32_t z_buffer_top;      /* R300_ZB_ZTOP: 0x4f14 */
};

/* The next several objects are not pure Radeon state; they inherit from
 * various Gallium classes. */

struct r300_constant_buffer {
    /* Buffer of constants */
    float constants[256][4];
    /* Total number of constants */
    unsigned count;
};

/* Query object.
 *
 * This is not a subclass of pipe_query because pipe_query is never
 * actually fully defined. So, rather than have it as a member, and do
 * subclass-style casting, we treat pipe_query as an opaque, and just
 * trust that our state tracker does not ever mess up query objects.
 */
struct r300_query {
    /* The kind of query. Currently only OQ is supported. */
    unsigned type;
    /* The current count of this query. Required to be at least 32 bits. */
    unsigned int count;
    /* The offset of this query into the query buffer, in bytes. */
    unsigned offset;
    /* if we've flushed the query */
    boolean flushed;
    /* if begin has been emitted */
    boolean begin_emitted;
    /* Linked list members. */
    struct r300_query* prev;
    struct r300_query* next;
};

struct r300_texture {
    /* Parent class */
    struct u_resource b;

    /* Offsets into the buffer. */
    unsigned offset[R300_MAX_TEXTURE_LEVELS];

    /* A pitch for each mip-level */
    unsigned pitch[R300_MAX_TEXTURE_LEVELS];

    /* A pitch multiplied by blockwidth as hardware wants
     * the number of pixels instead of the number of blocks. */
    unsigned hwpitch[R300_MAX_TEXTURE_LEVELS];

    /* Size of one zslice or face based on the texture target */
    unsigned layer_size[R300_MAX_TEXTURE_LEVELS];

    /* Whether the mipmap level is macrotiled. */
    enum r300_buffer_tiling mip_macrotile[R300_MAX_TEXTURE_LEVELS];

    /**
     * If non-zero, override the natural texture layout with
     * a custom stride (in bytes).
     *
     * \note Mipmapping fails for textures with a non-natural layout!
     *
     * \sa r300_texture_get_stride
     */
    unsigned stride_override;

    /* Total size of this texture, in bytes. */
    unsigned size;

    /* Whether this texture has non-power-of-two dimensions
     * or a user-specified pitch.
     * It can be either a regular texture or a rectangle one.
     */
    boolean uses_pitch;

    /* Pipe buffer backing this texture. */
    struct r300_winsys_buffer *buffer;

    /* Registers carrying texture format data. */
    /* Only format-independent bits should be filled in. */
    struct r300_texture_format_state tx_format;
    /* All bits should be filled in. */
    struct r300_texture_fb_state fb_state;

    /* Buffer tiling */
    enum r300_buffer_tiling microtile, macrotile;
};

struct r300_vertex_element_state {
    unsigned count;
    struct pipe_vertex_element velem[PIPE_MAX_ATTRIBS];

    struct r300_vertex_stream_state vertex_stream;
};

struct r300_context {
    /* Parent class */
    struct pipe_context context;

    /* Emission of drawing packets. */
    void (*emit_draw_arrays_immediate)(
            struct r300_context *r300,
            unsigned mode, unsigned start, unsigned count);

    void (*emit_draw_arrays)(
            struct r300_context *r300,
            unsigned mode, unsigned count);

    void (*emit_draw_elements)(
            struct r300_context *r300, struct pipe_resource* indexBuffer,
            unsigned indexSize, unsigned minIndex, unsigned maxIndex,
            unsigned mode, unsigned start, unsigned count);


    /* The interface to the windowing system, etc. */
    struct r300_winsys_screen *rws;
    /* Screen. */
    struct r300_screen *screen;
    /* Draw module. Used mostly for SW TCL. */
    struct draw_context* draw;
    /* Accelerated blit support. */
    struct blitter_context* blitter;

    /* Vertex buffer for rendering. */
    struct pipe_resource* vbo;
    /* Offset into the VBO. */
    size_t vbo_offset;

    /* Occlusion query buffer. */
    struct pipe_resource* oqbo;
    /* Query list. */
    struct r300_query *query_current;
    struct r300_query query_list;

    /* Various CSO state objects. */
    /* Beginning of atom list. */
    struct r300_atom atom_list;
    /* Blend state. */
    struct r300_atom blend_state;
    /* Blend color state. */
    struct r300_atom blend_color_state;
    /* User clip planes. */
    struct r300_atom clip_state;
    /* Depth, stencil, and alpha state. */
    struct r300_atom dsa_state;
    /* Fragment shader. */
    struct r300_atom fs;
    /* Fragment shader RC_CONSTANT_STATE variables. */
    struct r300_atom fs_rc_constant_state;
    /* Fragment shader constant buffer. */
    struct r300_atom fs_constants;
    /* Framebuffer state. */
    struct r300_atom fb_state;
    /* Occlusion query. */
    struct r300_atom query_start;
    /* Rasterizer state. */
    struct r300_atom rs_state;
    /* RS block state + VAP (vertex shader) output mapping state. */
    struct r300_atom rs_block_state;
    /* Scissor state. */
    struct r300_atom scissor_state;
    /* Textures state. */
    struct r300_atom textures_state;
    /* Vertex stream formatting state. */
    struct r300_atom vertex_stream_state;
    /* Vertex shader. */
    struct r300_atom vs_state;
    /* Vertex shader constant buffer. */
    struct r300_atom vs_constants;
    /* Viewport state. */
    struct r300_atom viewport_state;
    /* ZTOP state. */
    struct r300_atom ztop_state;
    /* PVS flush. */
    struct r300_atom pvs_flush;
    /* Texture cache invalidate. */
    struct r300_atom texture_cache_inval;

    /* Invariant state. This must be emitted to get the engine started. */
    struct r300_atom invariant_state;

    /* Vertex buffers for Gallium. */
    struct pipe_vertex_buffer vertex_buffer[PIPE_MAX_ATTRIBS];
    int vertex_buffer_count;
    int vertex_buffer_max_index;
    /* Vertex elements for Gallium. */
    struct r300_vertex_element_state *velems;
    bool any_user_vbs;

    /* Vertex info for Draw. */
    struct vertex_info vertex_info;

    struct pipe_stencil_ref stencil_ref;

    struct pipe_clip_state clip;

    struct pipe_viewport_state viewport;

    /* Stream locations for SWTCL. */
    int stream_loc_notcl[16];

    /* Flag indicating whether or not the HW is dirty. */
    uint32_t dirty_hw;
    /* Whether polygon offset is enabled. */
    boolean polygon_offset_enabled;
    /* Z buffer bit depth. */
    uint32_t zbuffer_bpp;
    /* Whether rendering is conditional and should be skipped. */
    boolean skip_rendering;
    /* Whether the two-sided stencil ref value is different for front and
     * back faces, and fallback should be used for r3xx-r4xx. */
    boolean stencil_ref_bf_fallback;
    /* Point sprites texcoord index,  1 bit per texcoord */
    int sprite_coord_enable;
    /* Whether two-sided color selection is enabled (AKA light_twoside). */
    boolean two_sided_color;

    /* upload managers */
    struct u_upload_mgr *upload_vb;
    struct u_upload_mgr *upload_ib;

    /* Stat counter. */
    uint64_t flush_counter;
};

/* Convenience cast wrapper. */
static INLINE struct r300_texture* r300_texture(struct pipe_resource* tex)
{
    return (struct r300_texture*)tex;
}

static INLINE struct r300_context* r300_context(struct pipe_context* context)
{
    return (struct r300_context*)context;
}

static INLINE struct r300_fragment_shader *r300_fs(struct r300_context *r300)
{
    return (struct r300_fragment_shader*)r300->fs.state;
}

struct pipe_context* r300_create_context(struct pipe_screen* screen,
                                         void *priv);

/* Context initialization. */
struct draw_stage* r300_draw_stage(struct r300_context* r300);
void r300_init_state_functions(struct r300_context* r300);
void r300_init_resource_functions(struct r300_context* r300);

static INLINE boolean CTX_DBG_ON(struct r300_context * ctx, unsigned flags)
{
    return SCREEN_DBG_ON(ctx->screen, flags);
}

static INLINE void CTX_DBG(struct r300_context * ctx, unsigned flags,
                       const char * fmt, ...)
{
    if (CTX_DBG_ON(ctx, flags)) {
        va_list va;
        va_start(va, fmt);
        vfprintf(stderr, fmt, va);
        va_end(va);
    }
}

#define DBG_ON  CTX_DBG_ON
#define DBG     CTX_DBG

#endif /* R300_CONTEXT_H */
