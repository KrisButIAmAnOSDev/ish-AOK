// The AOK graphics channel: a guest program describes what to draw, the host
// draws it.
//
// This exists because the alternative is worse. A guest drawing into a
// framebuffer does every pixel in emulated code; a guest talking to a Wayland
// compositor over VNC pays a composite, an encode and a decode per frame. Here
// the guest sends a few hundred small commands and the host renders them with
// Metal, so the only thing emulated is the program's own logic.
//
// The command vocabulary is deliberately NOT general. It matches PSCAL's own
// drawing abstraction -- Borland-style 2D primitives and fixed-function 3D --
// because that is what makes it fast: one command per DrawLine, not a
// serialised graphics API. Generality lives in the layers around it (the
// surface, the input stream, this transport), which is where a future
// compositor or framebuffer would plug in. See docs/book/ch30-roots.md's
// neighbours for the reasoning; this header is the contract.
#ifndef FS_AOKGFX_H
#define FS_AOKGFX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Bumped when the wire format changes in a way an older peer cannot parse.
//
// This is load-bearing, not ceremony: the guest half of this protocol ships in
// a ROOT FILESYSTEM, which is downloaded independently of the app. An old
// rootfs WILL meet a new iSH-AOK and vice versa -- the same skew that made an
// installed build unable to see a newly published rootfs. Without a handshake
// that mismatch shows up as garbled geometry; with one it shows up as a
// sentence telling the user which half to update.
#define AOKGFX_PROTOCOL_VERSION 1

// Every command is this header followed by `len - sizeof(header)` payload
// bytes. `len` counts the header, so a reader that meets an opcode it does not
// know can skip the whole command and keep going -- which is what lets a newer
// guest talk to an older host for anything additive.
//
// Little-endian and naturally aligned, because both ends are arm64/x86 and
// pretending otherwise would be a fiction nothing tests.
struct aokgfx_cmd {
    uint16_t op;       // AOKGFX_OP_*
    uint16_t len;      // total size of this command, header included
    uint32_t surface;  // which surface it applies to; 0 is the default one
};

// Surfaces are addressed by id from the start even though PSCAL only ever asks
// for one. A compositor needs many, and retrofitting an id into a shipped wire
// format is a break; carrying one now is four bytes.
#define AOKGFX_SURFACE_DEFAULT 0

#define AOKGFX_CMD_MAX 4096   // largest single command, including header

enum {
    // --- session ------------------------------------------------------
    // HELLO must be the first command written. Payload: aokgfx_hello.
    AOKGFX_OP_HELLO = 0x0001,
    AOKGFX_OP_SURFACE_CREATE,   // payload: aokgfx_surface_create
    AOKGFX_OP_SURFACE_DESTROY,
    // The frame boundary, and the only command the guest should block on: the
    // host presents, then releases the writer. Everything before it may be
    // batched and reordered into one submission.
    AOKGFX_OP_PRESENT,

    // --- 2D, the Borland-style primitives the frontends already expose ---
    AOKGFX_OP_CLEAR = 0x0100,   // payload: aokgfx_color
    AOKGFX_OP_SET_COLOR,        // payload: aokgfx_color
    AOKGFX_OP_PUT_PIXEL,        // payload: aokgfx_point
    AOKGFX_OP_DRAW_LINE,        // payload: aokgfx_rect (x0,y0,x1,y1)
    AOKGFX_OP_DRAW_RECT,        // payload: aokgfx_rect
    AOKGFX_OP_FILL_RECT,        // payload: aokgfx_rect
    AOKGFX_OP_DRAW_CIRCLE,      // payload: aokgfx_circle
    AOKGFX_OP_FILL_CIRCLE,      // payload: aokgfx_circle
    AOKGFX_OP_SET_LINE_WIDTH,   // payload: uint32 width
    AOKGFX_OP_DRAW_TEXT,        // payload: aokgfx_text + UTF-8 bytes

    // --- 3D: fixed-function, because that is what the language exposes ---
    // Immediate mode arrives as ONE command carrying the whole vertex run, not
    // a command per vertex. A particle demo emits thousands of vertices per
    // frame and a syscall each would be slower than the VNC path this
    // replaces, so the guest accumulates between Begin and End and submits the
    // run. That is why there is no AOKGFX_OP_VERTEX.
    AOKGFX_OP_PRIMITIVES = 0x0200,  // payload: aokgfx_primitives + vertices
    AOKGFX_OP_MATRIX_MODE,          // payload: uint32 mode
    AOKGFX_OP_LOAD_IDENTITY,
    AOKGFX_OP_PUSH_MATRIX,
    AOKGFX_OP_POP_MATRIX,
    AOKGFX_OP_ROTATE,               // payload: aokgfx_vec4 (angle, x, y, z)
    AOKGFX_OP_TRANSLATE,            // payload: aokgfx_vec4 (x, y, z, unused)
    AOKGFX_OP_SCALE,                // payload: aokgfx_vec4 (x, y, z, unused)
    AOKGFX_OP_PERSPECTIVE,          // payload: aokgfx_vec4 (fovy, aspect, near, far)
    AOKGFX_OP_VIEWPORT,             // payload: aokgfx_rect
    AOKGFX_OP_SET_STATE,            // payload: aokgfx_state (enable/disable)
    AOKGFX_OP_LIGHT,                // payload: aokgfx_light
    AOKGFX_OP_MATERIAL,             // payload: aokgfx_material
    AOKGFX_OP_BLEND_FUNC,           // payload: aokgfx_blend
    AOKGFX_OP_SHADE_MODEL,          // payload: uint32 (0 flat, 1 smooth)
    AOKGFX_OP_CLEAR_DEPTH,          // payload: float
};

// Matrix stacks, mirroring what the frontends' MatrixMode already means.
enum { AOKGFX_MATRIX_MODELVIEW = 0, AOKGFX_MATRIX_PROJECTION = 1 };

// Primitive topologies. A deliberate subset: these are the ones the 3D demos
// actually use, and an unknown topology is refused rather than guessed at.
enum {
    AOKGFX_PRIM_POINTS = 0,
    AOKGFX_PRIM_LINES,
    AOKGFX_PRIM_LINE_STRIP,
    AOKGFX_PRIM_LINE_LOOP,
    AOKGFX_PRIM_TRIANGLES,
    AOKGFX_PRIM_TRIANGLE_STRIP,
    AOKGFX_PRIM_TRIANGLE_FAN,
    AOKGFX_PRIM_QUADS,          // expanded to triangles by the host
    AOKGFX_PRIM__COUNT,
};

// Toggleable pipeline state, as a bitmask so one command can carry a batch.
enum {
    AOKGFX_STATE_DEPTH_TEST  = 1u << 0,
    AOKGFX_STATE_DEPTH_WRITE = 1u << 1,
    AOKGFX_STATE_CULL_FACE   = 1u << 2,
    AOKGFX_STATE_BLEND       = 1u << 3,
    AOKGFX_STATE_LIGHTING    = 1u << 4,
    AOKGFX_STATE_COLOR_MATERIAL = 1u << 5,
};

struct aokgfx_hello {
    uint32_t version;   // AOKGFX_PROTOCOL_VERSION the guest was built against
    uint32_t flags;     // reserved, must be zero
};

struct aokgfx_surface_create {
    uint32_t width, height;
};

struct aokgfx_color { float r, g, b, a; };
struct aokgfx_point { int32_t x, y; };
struct aokgfx_rect  { int32_t x0, y0, x1, y1; };
struct aokgfx_circle { int32_t x, y; uint32_t radius; };
struct aokgfx_vec4  { float x, y, z, w; };

struct aokgfx_text {
    int32_t x, y;
    uint32_t bytes;     // UTF-8 length following this struct
};

// One vertex of an immediate-mode run. Colour and normal travel per vertex
// because that is how Begin/End blocks set them -- the host does not have to
// track "current colour" across a run, which keeps its state machine small.
struct aokgfx_vertex {
    float x, y, z;
    float nx, ny, nz;
    float r, g, b, a;
};

struct aokgfx_primitives {
    uint32_t topology;  // AOKGFX_PRIM_*
    uint32_t count;     // vertices following this struct
};

struct aokgfx_state { uint32_t enable, disable; };

struct aokgfx_light {
    uint32_t index;
    struct aokgfx_vec4 position;
    struct aokgfx_color ambient, diffuse, specular;
};

struct aokgfx_material {
    struct aokgfx_color ambient, diffuse, specular;
    float shininess;
    float _pad[3];
};

struct aokgfx_blend { uint32_t src, dst; };

// --- events, guest-readable ------------------------------------------------
//
// Deliberately generic: key and pointer events, not answers to PollKey. The
// guest turns these into whatever its language exposes, which means evdev or a
// Wayland seat can later feed the same stream without the host learning a
// second input vocabulary.
enum {
    AOKGFX_EV_HELLO = 0x8001,   // payload: aokgfx_hello (the version in force)
    AOKGFX_EV_KEY   = 0x8002,   // payload: aokgfx_key_event
    AOKGFX_EV_POINTER,          // payload: aokgfx_pointer_event
    AOKGFX_EV_RESIZE,           // payload: aokgfx_surface_create
    AOKGFX_EV_CLOSE,            // the surface went away; stop drawing
};

enum {
    AOKGFX_MOD_SHIFT = 1u << 0,
    AOKGFX_MOD_CTRL  = 1u << 1,
    AOKGFX_MOD_ALT   = 1u << 2,
    AOKGFX_MOD_META  = 1u << 3,
};

struct aokgfx_key_event {
    uint32_t keycode;   // Linux input-event-codes KEY_*, so evdev can feed this
    uint32_t modifiers; // AOKGFX_MOD_*
    uint32_t pressed;   // 1 down, 0 up
    uint32_t unicode;   // 0 when the key has no character
};

struct aokgfx_pointer_event {
    int32_t x, y;
    uint32_t buttons;   // bit per button, 1 = down
    int32_t scroll_x, scroll_y;
};

// --- host side -------------------------------------------------------------
//
// The renderer registers here. It is separate from this file on purpose: the
// kernel owns framing, validation and the version handshake, so the protocol
// is enforced identically whether the consumer is the app's Metal renderer, a
// test harness, or nothing at all. With no backend the device still accepts
// and validates writes and discards them, which is what makes the wire format
// testable from ish-cli with no UI in the picture.
struct aokgfx_backend {
    void *ctx;
    // One validated batch: a whole write's worth of complete commands. The
    // callee must not retain `cmds` past the call.
    void (*submit)(void *ctx, const void *cmds, size_t len);
    // The guest closed its last handle. Tear the surface down.
    void (*closed)(void *ctx);
};

// NULL unregisters. Returns 0, or _EBUSY if a backend is already installed.
int aokgfx_set_backend(const struct aokgfx_backend *backend);

// Queue an input event for the guest to read. Safe to call from the UI thread.
void aokgfx_post_event(uint16_t op, const void *payload, size_t payload_len);

#endif
