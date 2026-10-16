// AceTeam Status: a pocket dashboard for live agent / PR / CI / balance state.
//
// The companion to the Beacon. The device registers a CLI command
// `aceteam_status`. A host (the bridge, or you in a serial terminal) runs it and
// then pushes snapshots of status; the Flipper renders them as a compact,
// scrollable list until the host disconnects or you press Back.
//
// On-device demo: from the idle screen press OK to load a canned snapshot with
// no host, so you can see the layout by hand.
//
// What this is (be precise): a read-only mirror of whatever the host chose to
// push, over the USB link the host already trusts. It does not verify the
// numbers, authenticate the host, or prove the state is current. It is a
// display, not an attestation. Signing of pushed state would ride on the same
// L3 work as the Beacon (see the README ladder).
//
// Wire protocol (host -> device), line oriented, each line ends with '\n':
//   one item per line:  <kind><space><text>
//     kind: h=header  g=good  r=bad  w=warn  i=info
//   a line that is a single '.' ends the snapshot and triggers a redraw.
//   the host may push further snapshots to update the screen.
//
// Build:  ufbt            (produces dist/aceteam_status.fap)
// Run:    ufbt launch
// Drive:  bridge/aceteam_status_bridge.py --port <serial>   (pushes mock status)

#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <cli/cli.h>
#include <toolbox/pipe.h>
#include <string.h>
#include <stdio.h>

#define APP_NAME "aceteam_status"
#define MAX_ITEMS 24
#define ITEM_TEXT 48
#define LINE_MAX 64
#define VISIBLE_ROWS 4
#define ROW_Y0 24
#define ROW_STEP 11

typedef struct {
    char kind; // h g r w i
    char text[ITEM_TEXT];
} StatusItem;

typedef enum {
    StatusEventInput, // a button was pressed
    StatusEventChanged, // the snapshot or the connection state changed
} StatusEventType;

typedef struct {
    StatusEventType type;
    InputEvent input;
} StatusEvent;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* events;
    CliRegistry* cli;
    FuriMutex* mutex; // guards the fields below (read by the draw thread)

    StatusItem items[MAX_ITEMS]; // the drawn snapshot
    size_t count;
    size_t scroll; // index of the top visible row
    bool have_snapshot; // at least one snapshot has been shown
    bool connected; // a host handler is live and pushing
    bool is_demo; // the shown snapshot is the on-device canned one

    bool cli_running; // a CLI handler is live (gates safe teardown)
    bool shutting_down; // app is exiting; new handlers return at once
} Status;

static char kind_glyph(char kind) {
    switch(kind) {
    case 'g':
        return '+';
    case 'r':
        return '!';
    case 'w':
        return '?';
    case 'i':
        return '-';
    default:
        return ' ';
    }
}

static void status_draw_callback(Canvas* canvas, void* ctx) {
    Status* s = ctx;
    canvas_clear(canvas);

    // Copy only the visible window out from under the lock, not the whole
    // array: this runs on the GUI service thread, whose stack is small.
    StatusItem win[VISIBLE_ROWS];
    size_t win_n = 0;
    furi_mutex_acquire(s->mutex, FuriWaitForever);
    size_t count = s->count;
    size_t scroll = s->scroll;
    bool have = s->have_snapshot;
    bool connected = s->connected;
    bool is_demo = s->is_demo;
    for(size_t row = 0; row < VISIBLE_ROWS; row++) {
        size_t idx = scroll + row;
        if(idx >= count) break;
        win[win_n++] = s->items[idx];
    }
    furi_mutex_release(s->mutex);

    // title bar
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "AceTeam Status");
    canvas_set_font(canvas, FontSecondary);
    const char* tag = connected ? "live" : (is_demo ? "demo" : "off");
    canvas_draw_str_aligned(canvas, 126, 10, AlignRight, AlignBottom, tag);
    canvas_draw_line(canvas, 0, 13, 128, 13);

    if(!have) {
        canvas_draw_str(canvas, 2, 30, "Waiting for host...");
        canvas_draw_str(canvas, 2, 62, "OK=demo   Back=exit");
        return;
    }

    for(size_t row = 0; row < win_n; row++) {
        int y = ROW_Y0 + (int)row * ROW_STEP;
        StatusItem* it = &win[row];
        if(it->kind == 'h') {
            canvas_set_font(canvas, FontPrimary);
            canvas_draw_str(canvas, 2, y, it->text);
            canvas_set_font(canvas, FontSecondary);
        } else {
            char line[ITEM_TEXT + 4];
            snprintf(line, sizeof(line), "%c %s", kind_glyph(it->kind), it->text);
            canvas_draw_str(canvas, 2, y, line);
        }
    }

    // scroll affordance
    if(count > VISIBLE_ROWS) {
        if(scroll > 0) canvas_draw_str_aligned(canvas, 126, ROW_Y0 - 2, AlignRight, AlignBottom, "^");
        if(scroll + VISIBLE_ROWS < count)
            canvas_draw_str_aligned(canvas, 126, 62, AlignRight, AlignBottom, "v");
    }
}

static void status_input_callback(InputEvent* input, void* ctx) {
    Status* s = ctx;
    StatusEvent ev = {.type = StatusEventInput, .input = *input};
    furi_message_queue_put(s->events, &ev, FuriWaitForever);
}

static void post_changed(Status* s) {
    StatusEvent ev = {.type = StatusEventChanged};
    furi_message_queue_put(s->events, &ev, FuriWaitForever);
}

// Read one '\n'-terminated line from the pipe into buf (NUL-terminated, '\r'
// stripped). Returns false if the pipe broke or an ETX byte (0x03) arrived,
// which both mean the host is gone.
static bool read_line(PipeSide* pipe, char* buf, size_t cap) {
    size_t n = 0;
    for(;;) {
        char c;
        if(pipe_receive(pipe, &c, 1) != 1) return false; // pipe broken
        if(c == 0x03) return false; // host sent ETX / Ctrl-C
        if(c == '\r') continue;
        if(c == '\n') break;
        if(n < cap - 1) buf[n++] = c;
    }
    buf[n] = '\0';
    return true;
}

// Commit a freshly built snapshot into the shared, drawn buffer.
static void commit_snapshot(Status* s, StatusItem* build, size_t n) {
    furi_mutex_acquire(s->mutex, FuriWaitForever);
    memcpy(s->items, build, sizeof(StatusItem) * n);
    s->count = n;
    if(s->scroll > n) s->scroll = 0;
    s->have_snapshot = true;
    s->is_demo = false;
    furi_mutex_release(s->mutex);
    post_changed(s);
}

// Runs in a CLI-spawned thread. Reads pushed snapshots until the host goes.
static void status_cli(PipeSide* pipe, FuriString* args, void* context) {
    UNUSED(args);
    Status* s = context;

    furi_mutex_acquire(s->mutex, FuriWaitForever);
    bool shutting_down = s->shutting_down;
    if(!shutting_down) {
        s->cli_running = true;
        s->connected = true;
    }
    furi_mutex_release(s->mutex);
    if(shutting_down) {
        const char* msg = "ERROR shutting down\r\n";
        pipe_send(pipe, msg, strlen(msg));
        return;
    }
    post_changed(s);

    const char* hello = "READY push items; '.' ends a snapshot; Ctrl-C to stop\r\n";
    pipe_send(pipe, hello, strlen(hello));

    // Heap, not stack: this runs on a CLI service thread with a small stack.
    StatusItem* build = malloc(sizeof(StatusItem) * MAX_ITEMS);
    size_t n = 0;
    char line[LINE_MAX];
    while(read_line(pipe, line, sizeof(line))) {
        if(line[0] == '.' && line[1] == '\0') {
            commit_snapshot(s, build, n);
            n = 0;
            continue;
        }
        if(line[0] == '\0') continue; // skip blank lines
        if(n >= MAX_ITEMS) continue; // ignore overflow past the cap
        build[n].kind = line[0];
        const char* text = (line[1] == ' ') ? line + 2 : line + 1;
        strncpy(build[n].text, text, ITEM_TEXT);
        build[n].text[ITEM_TEXT - 1] = '\0';
        n++;
    }

    free(build);

    furi_mutex_acquire(s->mutex, FuriWaitForever);
    s->connected = false;
    s->cli_running = false;
    furi_mutex_release(s->mutex);
    post_changed(s);
}

// A canned snapshot so the idle screen OK press shows the layout with no host.
static void load_demo(Status* s) {
    static const StatusItem demo[] = {
        {'h', "Waves"},
        {'g', "3 agents running"},
        {'w', "2 PRs awaiting review"},
        {'h', "CI"},
        {'g', "main green"},
        {'r', "staging red (1 job)"},
        {'h', "Account"},
        {'i', "ACET balance 142.5"},
    };
    size_t n = sizeof(demo) / sizeof(demo[0]);
    furi_mutex_acquire(s->mutex, FuriWaitForever);
    memcpy(s->items, demo, sizeof(demo));
    s->count = n;
    s->scroll = 0;
    s->have_snapshot = true;
    s->is_demo = true;
    furi_mutex_release(s->mutex);
    post_changed(s);
}

int32_t aceteam_status_app(void* p) {
    UNUSED(p);
    Status* s = malloc(sizeof(Status));
    memset(s, 0, sizeof(Status));
    s->events = furi_message_queue_alloc(16, sizeof(StatusEvent));
    s->mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    s->view_port = view_port_alloc();
    view_port_draw_callback_set(s->view_port, status_draw_callback, s);
    view_port_input_callback_set(s->view_port, status_input_callback, s);

    s->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(s->gui, s->view_port, GuiLayerFullscreen);

    s->cli = furi_record_open(RECORD_CLI);
    // ParallelSafe is required or the CLI refuses the command while this app is
    // open, and this app is always open while it handles the command.
    cli_registry_add_command(s->cli, APP_NAME, CliCommandFlagParallelSafe, status_cli, s);

    StatusEvent ev;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(s->events, &ev, FuriWaitForever) != FuriStatusOk) continue;

        if(ev.type == StatusEventChanged) {
            view_port_update(s->view_port);
            continue;
        }

        if(ev.input.type != InputTypePress) continue;

        furi_mutex_acquire(s->mutex, FuriWaitForever);
        bool have = s->have_snapshot;
        size_t count = s->count;
        furi_mutex_release(s->mutex);

        switch(ev.input.key) {
        case InputKeyBack:
            running = false;
            break;
        case InputKeyOk:
            if(!have) load_demo(s);
            break;
        case InputKeyUp:
            furi_mutex_acquire(s->mutex, FuriWaitForever);
            if(s->scroll > 0) s->scroll--;
            furi_mutex_release(s->mutex);
            view_port_update(s->view_port);
            break;
        case InputKeyDown:
            furi_mutex_acquire(s->mutex, FuriWaitForever);
            if(count > VISIBLE_ROWS && s->scroll + VISIBLE_ROWS < count) s->scroll++;
            furi_mutex_release(s->mutex);
            view_port_update(s->view_port);
            break;
        default:
            break;
        }
    }

    // Stop new handlers, then wake and wait for any in-flight handler before
    // freeing state it reads. A pushing host keeps status_cli blocked in
    // read_line; deleting the command breaks its pipe so read_line returns.
    furi_mutex_acquire(s->mutex, FuriWaitForever);
    s->shutting_down = true;
    furi_mutex_release(s->mutex);

    cli_registry_delete_command(s->cli, APP_NAME);

    for(;;) {
        furi_mutex_acquire(s->mutex, FuriWaitForever);
        bool busy = s->cli_running;
        furi_mutex_release(s->mutex);
        if(!busy) break;
        furi_delay_ms(10);
    }

    furi_record_close(RECORD_CLI);

    gui_remove_view_port(s->gui, s->view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(s->view_port);

    furi_message_queue_free(s->events);
    furi_mutex_free(s->mutex);
    free(s);
    return 0;
}
