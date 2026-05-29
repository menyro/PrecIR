#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_cortex.h>
#include <furi_hal_infrared.h>

#include <dialogs/dialogs.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define PRECIR_EXTENSION ".precir"
#define PRECIR_BASE_PATH EXT_PATH("apps_data/precir")
#define PRECIR_JOBS_PATH EXT_PATH("apps_data/precir/jobs")
#define PRECIR_CARRIER_HZ 1000000U
#define PRECIR_DUTY_CYCLE 0.5f
#define PRECIR_FRAME_GAP_US 2000U

typedef enum {
    PrecirTxModeAuto,
    PrecirTxModeInternal,
    PrecirTxModeExternal,
} PrecirTxMode;

typedef struct {
    uint8_t* data;
    size_t size;
    uint16_t repeats;
} PrecirFrame;

typedef struct {
    const uint32_t* timings;
    size_t count;
    size_t index;
} PrecirRawTx;

typedef struct {
    ViewPort* view_port;
    FuriMessageQueue* event_queue;
    DialogsApp* dialogs;
    Storage* storage;
    FuriString* path;
    FuriString* name;
    char status[48];
    PrecirFrame* frames;
    size_t frame_count;
    size_t current_frame;
    bool loaded;
    bool pp16;
    bool transmitting;
    PrecirTxMode tx_mode;
} PrecirApp;

static const uint16_t precir_pp4_pause_us[4] = {61, 244, 122, 183};
static const uint16_t precir_pp16_pause_us[16] =
    {27, 51, 35, 43, 147, 123, 139, 131, 83, 59, 75, 67, 91, 115, 99, 107};

static char* precir_trim(char* value) {
    while(*value && isspace((unsigned char)*value)) value++;
    char* end = value + strlen(value);
    while(end > value && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return value;
}

static void precir_free_frames(PrecirApp* app) {
    if(app->frames) {
        for(size_t i = 0; i < app->frame_count; i++) {
            free(app->frames[i].data);
        }
        free(app->frames);
    }
    app->frames = NULL;
    app->frame_count = 0;
    app->current_frame = 0;
    app->loaded = false;
}

static bool precir_add_frame(PrecirApp* app, uint8_t* data, size_t size, uint16_t repeats) {
    PrecirFrame* frames = realloc(app->frames, sizeof(PrecirFrame) * (app->frame_count + 1));
    if(!frames) return false;
    app->frames = frames;
    app->frames[app->frame_count].data = data;
    app->frames[app->frame_count].size = size;
    app->frames[app->frame_count].repeats = repeats;
    app->frame_count++;
    return true;
}

static bool precir_parse_hex_bytes(const char* hex, uint8_t** out_data, size_t* out_size) {
    size_t length = strlen(hex);
    if((length == 0) || (length & 1)) return false;

    size_t size = length / 2;
    uint8_t* data = malloc(size);
    if(!data) return false;

    for(size_t i = 0; i < size; i++) {
        char chunk[3] = {hex[i * 2], hex[i * 2 + 1], '\0'};
        char* end = NULL;
        unsigned long value = strtoul(chunk, &end, 16);
        if(!end || *end != '\0' || value > 0xFF) {
            free(data);
            return false;
        }
        data[i] = value;
    }

    *out_data = data;
    *out_size = size;
    return true;
}

static bool precir_load_job(PrecirApp* app, const char* path) {
    File* file = storage_file_alloc(app->storage);
    bool ok = false;
    bool saw_filetype = false;
    bool saw_version = false;
    bool saw_protocol = false;
    bool pp16 = false;
    bool parse_failed = false;

    precir_free_frames(app);

    do {
        if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) break;

        uint64_t size64 = storage_file_size(file);
        if(size64 == 0 || size64 > (256 * 1024)) break;

        size_t size = (size_t)size64;
        char* buffer = malloc(size + 1);
        if(!buffer) break;

        if(storage_file_read(file, buffer, size) != size) {
            free(buffer);
            break;
        }
        buffer[size] = '\0';

        char* saveptr = NULL;
        for(char* line = strtok_r(buffer, "\r\n", &saveptr); line;
            line = strtok_r(NULL, "\r\n", &saveptr)) {
            char* trimmed = precir_trim(line);
            if((trimmed[0] == '\0') || (trimmed[0] == '#')) continue;

            if(strncmp(trimmed, "Filetype:", 9) == 0) {
                saw_filetype = strcmp(precir_trim(trimmed + 9), "PrecIR Job") == 0;
            } else if(strncmp(trimmed, "Version:", 8) == 0) {
                saw_version = strcmp(precir_trim(trimmed + 8), "1") == 0;
            } else if(strncmp(trimmed, "Protocol:", 9) == 0) {
                char* protocol = precir_trim(trimmed + 9);
                if(strcmp(protocol, "PP16") == 0) {
                    pp16 = true;
                    saw_protocol = true;
                } else if(strcmp(protocol, "PP4") == 0) {
                    pp16 = false;
                    saw_protocol = true;
                }
            } else if(strncmp(trimmed, "Frame:", 6) == 0) {
                char* value = precir_trim(trimmed + 6);
                char* end = NULL;
                unsigned long repeats = strtoul(value, &end, 10);
                if(!end || repeats == 0 || repeats > 0xFFFF) {
                    parse_failed = true;
                    break;
                }

                char* hex = precir_trim(end);
                uint8_t* frame_data = NULL;
                size_t frame_size = 0;
                if(!precir_parse_hex_bytes(hex, &frame_data, &frame_size) ||
                   !precir_add_frame(app, frame_data, frame_size, repeats)) {
                    free(frame_data);
                    parse_failed = true;
                    break;
                }
            }
        }

        free(buffer);
        ok = !parse_failed && saw_filetype && saw_version && saw_protocol && (app->frame_count > 0);
        if(ok) {
            app->pp16 = pp16;
            app->loaded = true;
            furi_string_set_str(app->path, path);
            const char* name = strrchr(path, '/');
            furi_string_set_str(app->name, name ? name + 1 : path);
            snprintf(
                app->status,
                sizeof(app->status),
                "Loaded %lu frame(s)",
                (unsigned long)app->frame_count);
        }
    } while(false);

    storage_file_close(file);
    storage_file_free(file);

    if(!ok) {
        precir_free_frames(app);
        snprintf(app->status, sizeof(app->status), "Failed to load job");
    }

    return ok;
}

static FuriHalInfraredTxGetDataState precir_raw_tx_callback(
    void* context,
    uint32_t* duration,
    bool* level) {
    PrecirRawTx* tx = context;
    *duration = tx->timings[tx->index];
    *level = ((tx->index & 1U) == 0U);
    tx->index++;
    return (tx->index >= tx->count) ? FuriHalInfraredTxGetDataStateLastDone :
                                      FuriHalInfraredTxGetDataStateOk;
}

static uint32_t* precir_build_timings(const PrecirFrame* frame, bool pp16, size_t* count) {
    size_t symbol_count = frame->size * (pp16 ? 2 : 4);
    size_t total = symbol_count * 2 + 2;
    uint32_t* timings = malloc(sizeof(uint32_t) * total);
    if(!timings) return NULL;

    const uint16_t burst = pp16 ? 21 : 40;
    size_t index = 0;

    for(size_t i = 0; i < frame->size; i++) {
        uint8_t value = frame->data[i];
        size_t steps = pp16 ? 2 : 4;
        for(size_t s = 0; s < steps; s++) {
            uint8_t symbol = pp16 ? (value & 0x0F) : (value & 0x03);
            timings[index++] = burst;
            timings[index++] = pp16 ? precir_pp16_pause_us[symbol] : precir_pp4_pause_us[symbol];
            value >>= pp16 ? 4 : 2;
        }
    }

    timings[index++] = burst;
    timings[index++] = PRECIR_FRAME_GAP_US;
    *count = index;
    return timings;
}

static FuriHalInfraredTxPin precir_resolve_pin(PrecirTxMode mode) {
    if(mode == PrecirTxModeInternal) return FuriHalInfraredTxPinInternal;
    if(mode == PrecirTxModeExternal) return FuriHalInfraredTxPinExtPA7;
    return furi_hal_infrared_detect_tx_output();
}

static const char* precir_pin_label(PrecirTxMode mode) {
    if(mode == PrecirTxModeInternal) return "Internal";
    if(mode == PrecirTxModeExternal) return "GPIO PA7";
    return "Auto";
}

static bool precir_transmit(PrecirApp* app) {
    if(!app->loaded || furi_hal_infrared_is_busy()) return false;

    furi_hal_infrared_set_tx_output(precir_resolve_pin(app->tx_mode));
    app->transmitting = true;

    bool success = true;
    for(size_t i = 0; i < app->frame_count && success; i++) {
        app->current_frame = i + 1;
        const PrecirFrame* frame = &app->frames[i];
        size_t timing_count = 0;
        uint32_t* timings = precir_build_timings(frame, app->pp16, &timing_count);
        if(!timings) {
            success = false;
            break;
        }

        for(uint16_t repeat = 0; repeat < frame->repeats; repeat++) {
            PrecirRawTx tx = {.timings = timings, .count = timing_count, .index = 0};
            furi_hal_infrared_async_tx_set_data_isr_callback(precir_raw_tx_callback, &tx);
            furi_hal_infrared_async_tx_start(PRECIR_CARRIER_HZ, PRECIR_DUTY_CYCLE);
            furi_hal_infrared_async_tx_wait_termination();
        }

        free(timings);
        view_port_update(app->view_port);
        furi_delay_ms(1);
    }

    app->transmitting = false;
    app->current_frame = 0;
    snprintf(
        app->status,
        sizeof(app->status),
        "%s with %s",
        success ? "Transmit complete" : "Transmit failed",
        precir_pin_label(app->tx_mode));
    return success;
}

static bool precir_choose_file(PrecirApp* app) {
    DialogsFileBrowserOptions options;
    dialog_file_browser_set_basic_options(&options, PRECIR_EXTENSION, NULL);
    options.base_path = PRECIR_JOBS_PATH;
    options.skip_assets = true;

    if(!dialog_file_browser_show(app->dialogs, app->path, app->path, &options)) {
        return false;
    }

    return precir_load_job(app, furi_string_get_cstr(app->path));
}

static void precir_draw_callback(Canvas* canvas, void* context) {
    PrecirApp* app = context;

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "PrecIR");

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 22, app->loaded ? furi_string_get_cstr(app->name) : "No job loaded");

    char line[48];
    snprintf(
        line,
        sizeof(line),
        "Protocol: %s  Pin: %s",
        app->pp16 ? "PP16" : "PP4",
        precir_pin_label(app->tx_mode));
    canvas_draw_str(canvas, 2, 32, line);

    if(app->transmitting) {
        snprintf(
            line,
            sizeof(line),
            "Sending frame %lu/%lu",
            (unsigned long)app->current_frame,
            (unsigned long)app->frame_count);
    } else if(app->loaded) {
        snprintf(line, sizeof(line), "Frames: %lu", (unsigned long)app->frame_count);
    } else {
        snprintf(line, sizeof(line), "OK/RIGHT: choose job");
    }
    canvas_draw_str(canvas, 2, 42, line);

    canvas_draw_str(canvas, 2, 52, app->status);
    canvas_draw_str(canvas, 2, 62, "OK send  U/D pin  BACK exit");
}

static void precir_input_callback(InputEvent* input_event, void* context) {
    FuriMessageQueue* queue = context;
    furi_message_queue_put(queue, input_event, FuriWaitForever);
}

int32_t precir_main(void* p) {
    UNUSED(p);

    PrecirApp app = {
        .event_queue = furi_message_queue_alloc(8, sizeof(InputEvent)),
        .view_port = view_port_alloc(),
        .dialogs = furi_record_open(RECORD_DIALOGS),
        .storage = furi_record_open(RECORD_STORAGE),
        .path = furi_string_alloc_set(PRECIR_JOBS_PATH),
        .name = furi_string_alloc(),
        .tx_mode = PrecirTxModeAuto,
    };
    snprintf(app.status, sizeof(app.status), "Place jobs in apps_data/precir/jobs");

    view_port_draw_callback_set(app.view_port, precir_draw_callback, &app);
    view_port_input_callback_set(app.view_port, precir_input_callback, app.event_queue);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, app.view_port, GuiLayerFullscreen);

    InputEvent event;
    bool running = true;
    while(running) {
        view_port_update(app.view_port);
        if(furi_message_queue_get(app.event_queue, &event, 100) != FuriStatusOk) continue;
        if((event.type != InputTypePress) && (event.type != InputTypeRepeat)) continue;

        switch(event.key) {
        case InputKeyOk:
            if(app.loaded) {
                precir_transmit(&app);
            } else {
                precir_choose_file(&app);
            }
            break;
        case InputKeyRight:
            precir_choose_file(&app);
            break;
        case InputKeyUp:
            app.tx_mode = (app.tx_mode == PrecirTxModeAuto) ? PrecirTxModeInternal :
                          (app.tx_mode == PrecirTxModeInternal) ? PrecirTxModeExternal :
                                                                 PrecirTxModeAuto;
            snprintf(app.status, sizeof(app.status), "Pin set to %s", precir_pin_label(app.tx_mode));
            break;
        case InputKeyDown:
            app.tx_mode = (app.tx_mode == PrecirTxModeAuto) ? PrecirTxModeExternal :
                          (app.tx_mode == PrecirTxModeExternal) ? PrecirTxModeInternal :
                                                                 PrecirTxModeAuto;
            snprintf(app.status, sizeof(app.status), "Pin set to %s", precir_pin_label(app.tx_mode));
            break;
        case InputKeyBack:
            running = false;
            break;
        default:
            break;
        }
    }

    precir_free_frames(&app);
    gui_remove_view_port(gui, app.view_port);
    view_port_free(app.view_port);
    furi_message_queue_free(app.event_queue);
    furi_string_free(app.path);
    furi_string_free(app.name);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_DIALOGS);
    return 0;
}
