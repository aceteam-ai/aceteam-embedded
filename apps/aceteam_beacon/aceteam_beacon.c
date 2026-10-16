// AceTeam Beacon: a physical approve/deny gate for agent actions.
//
// The device registers a CLI command `aceteam_approve <action>`. When a host
// (the bridge, or you in a serial terminal) runs it, the Flipper shows the
// action and blocks until a human presses OK (approve) or Back (deny). The
// decision is printed back over the same CLI link as a single `RESULT` line.
//
// Interactive on-device demo: from the idle screen, press OK to fire a LOCAL
// demo request (no host needed) so you can feel the approve/deny loop by hand.
//
// What this proves (be precise): a human physically pressed approve or deny on
// THIS device, on the USB link the host already trusts, at the time of the
// press. It does NOT prove the person understood the action, that the on-screen
// text matches what the host will actually execute, or that the link was not
// relayed. Cryptographic signing of the decision is phase 2 (see the README).
//
// Build:  ufbt            (produces dist/aceteam_beacon.fap)
// Run:    ufbt launch
// Drive:  in a serial terminal on the Flipper CLI, type:  aceteam_approve deploy prod

#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <cli/cli.h> // RECORD_CLI, CliRegistry, cli_registry_* , cli_command.h
#include <toolbox/pipe.h> // pipe_send, PipeSide
#include <string.h>
#include <stdio.h>

#define ACTION_MAX 96
#define APPROVE_CMD "aceteam_approve"

// AceTeam smiley logo, 40x40, 1-bit XBM (LSB-first). Generated from
// aceteam/public/images/aceteam-icon-512.png.
#define LOGO_W 40
#define LOGO_H 40
static const uint8_t aceteam_logo_bits[] = {
    0x3f, 0x00, 0x00, 0x00, 0xfc, 0x0f, 0x00, 0x00, 0x00, 0xf0, 0x07, 0x00, 0x00, 0x00, 0xe0, 0x03,
    0x00, 0x00, 0x00, 0xc0, 0x01, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x00, 0xc0,
    0x3f, 0x00, 0x00, 0x00, 0xe0, 0x0f, 0x00, 0x00, 0xe0, 0xe0, 0x01, 0x00, 0x00, 0xfe, 0xe0, 0x00,
    0x00, 0x80, 0xff, 0xe0, 0x00, 0x00, 0xc0, 0xff, 0xe1, 0x00, 0x00, 0xc0, 0xc3, 0xe1, 0x00, 0x00,
    0xc0, 0xc1, 0xe1, 0x01, 0x00, 0xc0, 0xc1, 0xc3, 0x01, 0x00, 0xc0, 0xbf, 0xc3, 0x01, 0x00, 0xc0,
    0xbf, 0xe3, 0x01, 0x00, 0x80, 0x9f, 0xff, 0xc0, 0x01, 0x00, 0x02, 0xff, 0xc0, 0x01, 0x00, 0x00,
    0x3e, 0xc0, 0x01, 0x00, 0x00, 0x00, 0xe0, 0x01, 0x00, 0x00, 0x00, 0xe0, 0x01, 0x00, 0x06, 0x00,
    0xe0, 0x00, 0x00, 0x0f, 0x00, 0xf0, 0x00, 0x00, 0x1f, 0x00, 0x78, 0x00, 0x00, 0x3e, 0x00, 0x7c,
    0x00, 0x00, 0xfc, 0x00, 0x3f, 0x00, 0x00, 0xf8, 0xff, 0x0f, 0x00, 0x00, 0xf0, 0xff, 0x07, 0x00,
    0x00, 0xc0, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x80, 0x01,
    0x00, 0x00, 0x00, 0x80, 0x03, 0x00, 0x00, 0x00, 0xc0, 0x07, 0x00, 0x00, 0x00, 0xe0, 0x0f, 0x00,
    0x00, 0x00, 0xf0, 0x3f, 0x00, 0x00, 0x00, 0xfc};

typedef enum {
    BeaconEventInput, // a button was pressed
    BeaconEventRequest, // CLI asked for an approval
    BeaconEventCancel, // CLI request aborted (host disconnected / Ctrl+C)
} BeaconEventType;

typedef enum {
    DecisionPending,
    DecisionApprove,
    DecisionDeny,
} Decision;

typedef struct {
    BeaconEventType type;
    InputEvent input;
    char action[ACTION_MAX];
} BeaconEvent;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* event_queue; // input + cli -> main loop
    FuriMessageQueue* decision_queue; // main loop -> cli handler
    CliRegistry* cli;
    FuriMutex* mutex; // guards the fields below (read by the draw thread)
    bool pending;
    bool pending_from_cli; // true: a CLI handler is blocked waiting for the decision
    char action[ACTION_MAX];
    uint32_t approvals;
    uint32_t denials;
} Beacon;

static void beacon_draw_callback(Canvas* canvas, void* ctx) {
    Beacon* b = ctx;
    canvas_clear(canvas);

    furi_mutex_acquire(b->mutex, FuriWaitForever);
    bool pending = b->pending;
    char action[ACTION_MAX];
    strncpy(action, b->action, ACTION_MAX);
    action[ACTION_MAX - 1] = '\0';
    uint32_t ap = b->approvals;
    uint32_t dn = b->denials;
    furi_mutex_release(b->mutex);

    if(pending) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 12, "Approve action?");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 28, action);
        canvas_draw_str(canvas, 2, 52, "OK = approve");
        canvas_draw_str(canvas, 2, 62, "Back = deny");
    } else {
        canvas_draw_xbm(canvas, 2, 12, LOGO_W, LOGO_H, aceteam_logo_bits);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 48, 22, "AceTeam");
        canvas_draw_str(canvas, 48, 36, "Beacon");
        canvas_set_font(canvas, FontSecondary);
        char line[24];
        snprintf(line, sizeof(line), "ok:%lu no:%lu", (unsigned long)ap, (unsigned long)dn);
        canvas_draw_str(canvas, 48, 50, line);
        canvas_draw_str(canvas, 2, 62, "OK=demo   Back=exit");
    }
}

static void beacon_input_callback(InputEvent* input, void* ctx) {
    Beacon* b = ctx;
    BeaconEvent ev = {.type = BeaconEventInput, .input = *input};
    furi_message_queue_put(b->event_queue, &ev, FuriWaitForever);
}

// Runs in a CLI-spawned thread. Blocks until the human decides or the host goes.
static void beacon_cli_approve(PipeSide* pipe, FuriString* args, void* context) {
    Beacon* b = context;

    // Drop any decision left over from a previous cycle that ended by host
    // disconnect at the same moment as a press. Without this reset the stale
    // decision would be consumed immediately, approving or denying an action
    // nobody saw.
    furi_message_queue_reset(b->decision_queue);

    BeaconEvent ev = {.type = BeaconEventRequest};
    const char* a = furi_string_get_cstr(args);
    if(a == NULL || *a == '\0') a = "(unspecified action)";
    strncpy(ev.action, a, ACTION_MAX);
    ev.action[ACTION_MAX - 1] = '\0';
    furi_message_queue_put(b->event_queue, &ev, FuriWaitForever);

    Decision d = DecisionPending;
    while(d == DecisionPending) {
        if(furi_message_queue_get(b->decision_queue, &d, 100) == FuriStatusOk) {
            break;
        }
        if(cli_is_pipe_broken_or_is_etx_next_char(pipe)) {
            BeaconEvent cancel = {.type = BeaconEventCancel};
            furi_message_queue_put(b->event_queue, &cancel, FuriWaitForever);
            const char* msg = "RESULT cancelled\r\n";
            pipe_send(pipe, msg, strlen(msg));
            return;
        }
    }

    const char* result = (d == DecisionApprove) ? "RESULT approve\r\n" : "RESULT deny\r\n";
    pipe_send(pipe, result, strlen(result));
}

// Resolve a decision made on the device. Posts to the CLI handler only when a
// real CLI request is what is pending (a local demo has no waiter).
static void beacon_resolve(Beacon* b, Decision d) {
    bool notify_cli;
    furi_mutex_acquire(b->mutex, FuriWaitForever);
    b->pending = false;
    notify_cli = b->pending_from_cli;
    if(d == DecisionApprove)
        b->approvals++;
    else
        b->denials++;
    furi_mutex_release(b->mutex);

    if(notify_cli) {
        furi_message_queue_put(b->decision_queue, &d, FuriWaitForever);
    }
    view_port_update(b->view_port);
}

int32_t aceteam_beacon_app(void* p) {
    UNUSED(p);
    Beacon* b = malloc(sizeof(Beacon));
    b->event_queue = furi_message_queue_alloc(8, sizeof(BeaconEvent));
    b->decision_queue = furi_message_queue_alloc(1, sizeof(Decision));
    b->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    b->pending = false;
    b->pending_from_cli = false;
    b->action[0] = '\0';
    b->approvals = 0;
    b->denials = 0;

    b->view_port = view_port_alloc();
    view_port_draw_callback_set(b->view_port, beacon_draw_callback, b);
    view_port_input_callback_set(b->view_port, beacon_input_callback, b);

    b->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(b->gui, b->view_port, GuiLayerFullscreen);

    b->cli = furi_record_open(RECORD_CLI);
    // ParallelSafe is required: without it the CLI refuses the command with
    // "this command cannot be run while an application is open", and this app
    // is always the open application while it is handling the command.
    cli_registry_add_command(b->cli, APPROVE_CMD, CliCommandFlagParallelSafe, beacon_cli_approve, b);

    BeaconEvent ev;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(b->event_queue, &ev, FuriWaitForever) != FuriStatusOk) {
            continue;
        }

        if(ev.type == BeaconEventRequest) {
            furi_mutex_acquire(b->mutex, FuriWaitForever);
            b->pending = true;
            b->pending_from_cli = true;
            strncpy(b->action, ev.action, ACTION_MAX);
            b->action[ACTION_MAX - 1] = '\0';
            furi_mutex_release(b->mutex);
            view_port_update(b->view_port);

        } else if(ev.type == BeaconEventCancel) {
            furi_mutex_acquire(b->mutex, FuriWaitForever);
            b->pending = false;
            b->pending_from_cli = false;
            furi_mutex_release(b->mutex);
            view_port_update(b->view_port);

        } else if(ev.type == BeaconEventInput) {
            if(ev.input.type != InputTypePress) continue;

            furi_mutex_acquire(b->mutex, FuriWaitForever);
            bool pend = b->pending;
            furi_mutex_release(b->mutex);

            if(pend) {
                if(ev.input.key == InputKeyOk) {
                    beacon_resolve(b, DecisionApprove);
                } else if(ev.input.key == InputKeyBack) {
                    beacon_resolve(b, DecisionDeny);
                }
            } else {
                if(ev.input.key == InputKeyOk) {
                    // fire a local demo request, no host involved
                    furi_mutex_acquire(b->mutex, FuriWaitForever);
                    b->pending = true;
                    b->pending_from_cli = false;
                    strncpy(b->action, "demo: deploy prod v1.482", ACTION_MAX);
                    b->action[ACTION_MAX - 1] = '\0';
                    furi_mutex_release(b->mutex);
                    view_port_update(b->view_port);
                } else if(ev.input.key == InputKeyBack) {
                    running = false;
                }
            }
        }
    }

    cli_registry_delete_command(b->cli, APPROVE_CMD);
    furi_record_close(RECORD_CLI);

    gui_remove_view_port(b->gui, b->view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(b->view_port);

    furi_message_queue_free(b->event_queue);
    furi_message_queue_free(b->decision_queue);
    furi_mutex_free(b->mutex);
    free(b);
    return 0;
}
