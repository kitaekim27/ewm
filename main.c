#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/randr.h>
#include <xcb/xcb.h>
#include <xcb/xcb_event.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_icccm.h>

#define MASK_TAG0 (0)
#define MASK_TAG1 (1)
#define MASK_TAG2 (2)
#define MASK_TAG3 (4)
#define MASK_TAG4 (8)
#define MASK_TAG5 (16)
#define MASK_TAG6 (32)
#define MASK_TAG7 (64)
#define MASK_TAG8 (128)
#define MASK_TAG9 (256)

#define container_of(Pointer, ContainerType, MemberName)                                        \
    ({                                                                                          \
        const typeof(((ContainerType *)0)->MemberName) *__member_ptr = (Pointer);               \
        (ContainerType *)((unsigned char *)__member_ptr - offsetof(ContainerType, MemberName)); \
    })

#define max(A, B) ((A) > (B) ? (A) : (B))
#define min(A, B) ((A) < (B) ? (A) : (B))

struct list_node {
    struct list_node *prev;
    struct list_node *next;
};

typedef struct list_node *list_head_t;

struct box {
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;
};

static inline bool box_compare(const struct box a, const struct box b)
{
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

struct client {
    char name[256];
    uint8_t tags;

    float min_aspect_ratio;
    float max_aspect_ratio;

    int32_t max_width;
    int32_t max_height;
    int32_t min_width;
    int32_t min_height;

    int32_t base_width;
    int32_t base_height;
    int32_t width_inc;
    int32_t height_inc;

    struct box box;
    uint16_t border_width;

    bool is_fixed;
    bool is_floating;
    bool is_fullscreen;
    bool is_urgent;
    bool never_focus;

    struct monitor *monitor;
    xcb_window_t window;

    struct list_node list_node;
};

static inline int16_t client_width(const struct client *const client)
{
    return client->box.width + 2 * client->border_width;
}

static inline int16_t client_height(const struct client *const client)
{
    return client->box.height + 2 * client->border_width;
}

struct layout {
    char name[32];
    void (*arrange)(const struct monitor *const);
};

struct monitor {
    xcb_randr_output_t output;
    uint8_t enabled_tags;

    float main_area_fraction;
    uint8_t main_area_win_num;

    struct box crtc_box;
    struct box box;

    uint16_t gap_top;
    uint16_t gap_bottom;
    uint16_t gap_left;
    uint16_t gap_right;

    uint64_t clients_num;
    list_head_t clients;
    struct client *focused_client;

    struct layout layouts[1];
    uint8_t current_layout_idx;

    struct list_node list_node;
};

static xcb_connection_t *global_xconnection = NULL;

enum { WM_PROTOCOLS, WM_DELETE_WINDOW, WM_STATE, WM_TAKE_FOCUS, WM_END };

static xcb_ewmh_connection_t *global_ewmh_connection;
static xcb_atom_t global_wm_atoms[WM_END];

static xcb_screen_t *global_screen = NULL;
static uint16_t global_screen_width = 0;
static uint16_t global_screen_height = 0;

static list_head_t global_monitors = NULL;
static struct monitor *global_focused_monitor = NULL;

const static uint32_t global_client_border_width = 8;
const static uint32_t global_client_unfocus_pixel = 0x000000;
const static uint32_t global_client_focus_pixel = 0x000000;

const static bool should_respect_size_hints = true;

void list_append(list_head_t *head, struct list_node *const node)
{
    if (head == NULL) {
        *head = node;
        node->prev = *head;
        node->next = *head;
        return;
    }
    struct list_node *cursor = NULL;
    while (cursor->next != NULL) {
        cursor = cursor->next;
    }
    node->prev = cursor;
    cursor->next = node;
    node->next = *head;
    return;
}

void list_remove(list_head_t *head, struct list_node *const node)
{
    node->prev->next = node->next;
    node->next->prev = node->prev;
}

int client_set_size_hints(struct client *const client)
{
    xcb_size_hints_t size_hints;
    if (xcb_icccm_get_wm_normal_hints_reply(
            global_xconnection, xcb_icccm_get_wm_normal_hints(global_xconnection, client->window),
            &size_hints, NULL) != 0) {
        return 1;
    }
    if (size_hints.flags & XCB_ICCCM_SIZE_HINT_BASE_SIZE) {
        client->base_width = size_hints.base_width;
        client->base_height = size_hints.base_height;
    }
    if (size_hints.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) {
        client->min_width = size_hints.min_width;
        client->min_height = size_hints.min_height;
    }
    if (size_hints.flags & XCB_ICCCM_SIZE_HINT_P_ASPECT) {
        client->min_aspect_ratio = (float)size_hints.min_aspect_num / size_hints.min_aspect_den;
        client->max_aspect_ratio = (float)size_hints.max_aspect_num / size_hints.max_aspect_den;
    }
    return 0;
}

struct box get_box_with_size_hints(const struct client *const client, const struct box box)
{
    struct box new_box = box;
    struct monitor *monitor = client->monitor;
    if (new_box.x >= monitor->box.x + monitor->box.width) {
        new_box.x = monitor->box.x + monitor->box.width - client->box.width;
    }
    if (new_box.y >= monitor->box.y + monitor->box.height) {
        new_box.y = monitor->box.y + monitor->box.height - client->box.height;
    }
    if (new_box.x + new_box.width < monitor->box.x) {
        new_box.x = monitor->box.x;
    }
    if (new_box.y + new_box.height < monitor->box.y) {
        new_box.y = monitor->box.y;
    }
    if (should_respect_size_hints == false && client->is_floating == false) {
        return new_box;
    }
    if (client->min_aspect_ratio <= 0 || client->max_aspect_ratio <= 0) {
        goto IGNORE_ASPECT_RATIO;
    }
    // Window managers that honor aspect ratios should take into account the base size in
    // determining the preferred window size. If a base size is provided along with the aspect ratio
    // fields, the base size should be subtracted from the window size prior to checking that the
    // aspect ratio falls in range. If a base size is not provided, nothing should be subtracted
    // from the window size. (The minimum size is not to be used in place of the base size for this
    // purpose.)
    if (client->base_width > 0 && client->base_height > 0) {
        new_box.width -= client->base_width;
        new_box.height -= client->base_height;
    }
    if ((float)new_box.width / new_box.height > client->max_aspect_ratio) {
        new_box.height = new_box.width / client->max_aspect_ratio;
    }
    if ((float)new_box.width / new_box.height < client->min_aspect_ratio) {
        new_box.height = new_box.width / client->min_aspect_ratio;
    }
IGNORE_ASPECT_RATIO:
    if (client->base_width <= 0 && client->base_height <= 0) {
        new_box.width -= client->base_width;
        new_box.height -= client->base_height;
    }
    if (client->width_inc != 0) {
        new_box.width -= new_box.width % client->width_inc;
    }
    if (client->height_inc != 0) {
        new_box.height -= new_box.height % client->height_inc;
    }
    new_box.width = max(new_box.width + client->base_width, client->min_width);
    new_box.height = max(new_box.height + client->base_height, client->min_height);
    return new_box;
}

struct client *client_create(struct monitor *monitor, xcb_window_t window, int16_t x, int16_t y,
                             uint16_t width, uint16_t height, int16_t border_width, uint8_t tags)
{
    struct client *new_client = (struct client *)calloc(1, sizeof(struct client));
    if (new_client == NULL) {
        return NULL;
    }
    new_client->monitor = monitor;
    new_client->window = window;
    if (client_set_size_hints(new_client) != 0) {
        free(new_client);
        return NULL;
    }
    struct box box = {x, y, width, height};
    new_client->box = box;
    new_client->border_width = border_width;
    new_client->tags = tags;
    return new_client;
}

void client_configure(const struct client *const client)
{
    xcb_configure_notify_event_t notify_event;

    notify_event.response_type = XCB_CONFIGURE_NOTIFY;
    notify_event.event = client->window;
    notify_event.window = client->window;
    notify_event.above_sibling = XCB_NONE;
    notify_event.x = client->box.x;
    notify_event.y = client->box.y;
    notify_event.width = client->box.width;
    notify_event.height = client->box.height;
    notify_event.border_width = client->border_width;
    notify_event.override_redirect = false;

    xcb_send_event(global_xconnection, false, client->window, XCB_EVENT_MASK_STRUCTURE_NOTIFY,
                   (char *)&notify_event);
}

void client_move_resize(struct client *client, int16_t x, int16_t y, uint16_t width,
                        uint16_t height)
{
    struct box box = {x, y, width, height};
    struct box new_box = get_box_with_size_hints(client, box);
    if (box_compare(new_box, box) == true) {
        return;
    }
    uint32_t values[] = {new_box.x, new_box.y, new_box.width, new_box.height};
    xcb_configure_window(global_xconnection, client->window,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |
                             XCB_CONFIG_WINDOW_HEIGHT,
                         values);
}

void monitor_tile(const struct monitor *const monitor)
{
    uint16_t main_area_width = monitor->clients_num <= monitor->main_area_win_num
                                   ? monitor->box.width
                                   : monitor->box.width * monitor->main_area_fraction;
    uint16_t main_win_height = monitor->box.height / monitor->main_area_win_num;

    uint64_t clients_idx = 0;
    struct list_node *clients_cursor = monitor->clients;
    while (clients_cursor != NULL && clients_idx < monitor->main_area_win_num) {
        struct client *client = container_of(clients_cursor, struct client, list_node);
        client_move_resize(client, monitor->box.x, monitor->box.y + main_win_height * clients_idx,
                           main_area_width, main_win_height);
        clients_cursor = clients_cursor->next;
        ++clients_idx;
    }

    uint64_t clients_left_num = monitor->clients_num - clients_idx;
    clients_idx = 0;
    uint16_t sub_area_width = monitor->box.width - main_area_width;
    uint16_t sub_win_height = monitor->box.height / clients_left_num;
    while (clients_cursor != NULL) {
        struct client *client = container_of(clients_cursor, struct client, list_node);
        client_move_resize(client, monitor->box.x + main_area_width,
                           monitor->box.y + sub_win_height * clients_idx, sub_area_width,
                           sub_win_height);
        clients_cursor = clients_cursor->next;
    }
}

struct monitor *monitor_create(xcb_randr_output_t output, int16_t crtc_x, int16_t crtc_y,
                               size_t crtc_width, size_t crtc_height)
{
    struct monitor *new_monitor = (struct monitor *)calloc(1, sizeof(struct monitor));
    if (new_monitor == NULL) {
        return NULL;
    }
    new_monitor->output = output;
    new_monitor->main_area_fraction = 0.6;
    new_monitor->main_area_win_num = 1;
    struct box crtc_box = {crtc_x, crtc_y, crtc_width, crtc_height};
    new_monitor->crtc_box = crtc_box;
    new_monitor->gap_top = 8;
    new_monitor->gap_bottom = 8;
    new_monitor->gap_left = 8;
    new_monitor->gap_right = 8;
    struct box box = {new_monitor->crtc_box.x + new_monitor->gap_left,
                      new_monitor->crtc_box.y + new_monitor->gap_top,
                      new_monitor->crtc_box.width - new_monitor->gap_right,
                      new_monitor->crtc_box.height - new_monitor->gap_bottom};
    new_monitor->box = box;
    struct layout layout_tile = {.name = "tile", .arrange = monitor_tile};
    new_monitor->layouts[0] = layout_tile;
    new_monitor->current_layout_idx = 0;
    return new_monitor;
}

void monitor_append_client(struct monitor *monitor, struct client *client)
{
    list_append(&monitor->clients, &client->list_node);
    ++monitor->clients_num;
}

void client_unfocus(const struct client *const client)
{
    uint32_t values[] = {global_client_border_width, global_client_unfocus_pixel};
    xcb_configure_window(global_xconnection, client->window,
                         XCB_CONFIG_WINDOW_BORDER_WIDTH | XCB_CW_BORDER_PIXEL, values);
}

void client_focus(const struct client *const client)
{
    uint32_t values[] = {global_client_border_width, global_client_focus_pixel};
    xcb_configure_window(global_xconnection, client->window,
                         XCB_CONFIG_WINDOW_BORDER_WIDTH | XCB_CW_BORDER_PIXEL, values);
}

void monitor_remove_client(struct monitor *monitor, struct client *client)
{
    list_remove(&monitor->clients, &client->list_node);
    --monitor->clients_num;
    if (monitor->clients == NULL || client != monitor->focused_client) {
        return;
    }
    monitor->focused_client = container_of(monitor->clients->prev, struct client, list_node);
    client_focus(monitor->focused_client);
}

struct client *get_client_by_win(xcb_window_t window)
{
    for (struct list_node *monitor_cursor = global_monitors; monitor_cursor != NULL;
         monitor_cursor = monitor_cursor->next) {
        struct monitor *monitor = container_of(monitor_cursor, struct monitor, list_node);
        for (struct list_node *client_cursor = monitor->clients; client_cursor != NULL;
             client_cursor = client_cursor->next) {
            struct client *client = container_of(client_cursor, struct client, list_node);
            if (client->window == window) {
                return client;
            }
        }
    }
    return NULL;
}

bool check_unique_crtc(xcb_randr_get_crtc_info_reply_t *crtc_info_reply)
{
    for (struct list_node *cursor = global_monitors; cursor != NULL; cursor = cursor->next) {
        struct monitor *monitor = container_of(cursor, struct monitor, list_node);
        if (monitor->crtc_box.width == crtc_info_reply->width &&
            monitor->crtc_box.height == crtc_info_reply->height &&
            monitor->crtc_box.x == crtc_info_reply->x &&
            monitor->crtc_box.y == crtc_info_reply->y) {
            return false;
        }
    }
    return true;
}

int update_monitors(void)
{
    xcb_randr_get_screen_resources_reply_t *screen_resources_reply =
        xcb_randr_get_screen_resources_reply(
            global_xconnection,
            xcb_randr_get_screen_resources(global_xconnection, global_screen->root), NULL);
    if (screen_resources_reply == NULL) {
        fprintf(stderr, "Can't get screen resources from X Server!\n");
        return 1;
    }

    size_t outputs_len = xcb_randr_get_screen_resources_outputs_length(screen_resources_reply);
    xcb_randr_output_t *outputs = xcb_randr_get_screen_resources_outputs(screen_resources_reply);
    xcb_randr_get_output_info_cookie_t *output_info_cookies =
        (xcb_randr_get_output_info_cookie_t *)calloc(outputs_len,
                                                     sizeof(xcb_randr_get_output_info_cookie_t));
    if (output_info_cookies == NULL) {
        return 1;
    }

    for (uint64_t i = 0; i < outputs_len; ++i) {
        output_info_cookies[i] =
            xcb_randr_get_output_info(global_xconnection, outputs[i], XCB_CURRENT_TIME);
    }

    for (uint64_t i = 0; i < outputs_len; ++i) {
        xcb_randr_get_output_info_reply_t *output_info_reply =
            xcb_randr_get_output_info_reply(global_xconnection, output_info_cookies[i], NULL);
        if (output_info_reply == NULL) {
            continue;
        }

        xcb_randr_get_crtc_info_reply_t *crtc_info_reply = xcb_randr_get_crtc_info_reply(
            global_xconnection,
            xcb_randr_get_crtc_info(global_xconnection, output_info_reply->crtc, XCB_CURRENT_TIME),
            NULL);
        if (crtc_info_reply == NULL || check_unique_crtc(crtc_info_reply) == false) {
            goto LOOP_CLEANUP;
        }

        list_append(&global_monitors,
                    &monitor_create(outputs[i], crtc_info_reply->x, crtc_info_reply->y,
                                    crtc_info_reply->width, crtc_info_reply->height)
                         ->list_node);

    LOOP_CLEANUP:
        free(crtc_info_reply);
        free(output_info_reply);
    }

    free(output_info_cookies);
    free(screen_resources_reply);

    // TODO: Update contents of each monitors.

    return 0;
}

int x11_randr_init(void)
{
    xcb_randr_get_output_primary_reply_t *primary_output_reply = xcb_randr_get_output_primary_reply(
        global_xconnection, xcb_randr_get_output_primary(global_xconnection, global_screen->root),
        NULL);
    // TODO: Return primary output to caller! (primary_output_reply->output).
    if (primary_output_reply == NULL) {
        fprintf(stderr, "Can't get primary output info from X Server!\n");
        return 1;
    }

    update_monitors();

    free(primary_output_reply);

    xcb_randr_select_input(global_xconnection, global_screen->root,
                           XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);
    xcb_set_input_focus(global_xconnection, XCB_INPUT_FOCUS_POINTER_ROOT, global_screen->root,
                        XCB_CURRENT_TIME);

    return 0;
}

xcb_atom_t get_atom(const char *const name)
{
    xcb_intern_atom_reply_t *intern_atom_reply = xcb_intern_atom_reply(
        global_xconnection, xcb_intern_atom(global_xconnection, 0, strlen(name), name), NULL);
    if (intern_atom_reply == NULL) {
        return XCB_ATOM_NONE;
    }
    xcb_atom_t result = intern_atom_reply->atom;
    free(intern_atom_reply);
    return result;
}

int x11_init(void)
{
    int32_t screen_num = 0;
    global_xconnection = xcb_connect(NULL, &screen_num);
    if (xcb_connection_has_error(global_xconnection)) {
        fprintf(stderr, "Can't connect to X Server!\n");
        return 1;
    }

    global_ewmh_connection = (xcb_ewmh_connection_t *)malloc(sizeof(xcb_ewmh_connection_t));
    if (xcb_ewmh_init_atoms_replies(global_ewmh_connection,
                                    xcb_ewmh_init_atoms(global_xconnection, global_ewmh_connection),
                                    NULL) != 0) {
        fprintf(stderr, "Can't initialize EWMH atoms!\n");
        return 1;
    }

    global_wm_atoms[WM_PROTOCOLS] = get_atom("WM_PROTOCOLS");
    global_wm_atoms[WM_DELETE_WINDOW] = get_atom("WM_DELETE_WINDOW");
    global_wm_atoms[WM_STATE] = get_atom("WM_STATE");
    global_wm_atoms[WM_TAKE_FOCUS] = get_atom("WM_TAKE_FOCUS");

    xcb_screen_iterator_t iterator = xcb_setup_roots_iterator(xcb_get_setup(global_xconnection));
    for (uint64_t i = 0; i < screen_num; ++i) {
        xcb_screen_next(&iterator);
    }
    global_screen = iterator.data;

    uint32_t event_mask[] = {(XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                              XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                              XCB_EVENT_MASK_BUTTON_PRESS)};
    xcb_generic_error_t *error = xcb_request_check(
        global_xconnection,
        xcb_change_window_attributes_checked(global_xconnection, global_screen->root,
                                             XCB_CW_EVENT_MASK, event_mask));
    if (error != NULL) {
        free(error);
        fprintf(stderr, "Can't register to X Server for reparenting!\n");
        fprintf(stderr, "Maybe there's another window manager already running?\n");
        return 1;
    }

    if (xcb_get_extension_data(global_xconnection, &xcb_randr_id)->present == 0) {
        fprintf(stderr, "Failed to get RandR extension!\n");
        return 1;
    }

    if (x11_randr_init() != 0) {
        fprintf(stderr, "RandR-specific initialization failed!\n");
        return 1;
    }

    return 0;
}

void handle_button_press(xcb_button_press_event_t *event) {}

void handle_client_message(xcb_client_message_event_t *event)
{
    struct client *client = get_client_by_win(event->window);
    if (client == NULL) {
        return;
    }
}

void handle_configure_notify(xcb_configure_notify_event_t *event)
{
    ;
}

void handle_configure_request(xcb_configure_request_event_t *event)
{
    struct client *client = get_client_by_win(event->window);
    if (client == NULL) {
        uint32_t values[7];
        uint8_t i = 0;
        if (event->value_mask & XCB_CONFIG_WINDOW_X) {
            values[i++] = event->x;
        }
        if (event->value_mask & XCB_CONFIG_WINDOW_Y) {
            values[i++] = event->y;
        }
        if (event->value_mask & XCB_CONFIG_WINDOW_WIDTH) {
            values[i++] = event->width;
        }
        if (event->value_mask & XCB_CONFIG_WINDOW_HEIGHT) {
            values[i++] = event->height;
        }
        if (event->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) {
            values[i++] = event->border_width;
        }
        if (event->value_mask & XCB_CONFIG_WINDOW_SIBLING) {
            values[i++] = event->sibling;
        }
        if (event->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) {
            values[i++] = event->stack_mode;
        }
        xcb_configure_window(global_xconnection, event->window, event->value_mask, values);
        return;
    }

    if (event->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) {
        client->border_width = event->border_width;
        return;
    }

    if (client->is_floating == false) {
        client_configure(client);
        return;
    }

    struct monitor *monitor = client->monitor;
    if (event->value_mask & XCB_CONFIG_WINDOW_X) {
        client->box.x = monitor->box.x + event->x;
    }
    if (event->value_mask & XCB_CONFIG_WINDOW_Y) {
        client->box.y = monitor->box.y + event->y;
    }
    if (event->value_mask & XCB_CONFIG_WINDOW_WIDTH) {
        client->box.width = monitor->box.width + event->width;
    }
    if (event->value_mask & XCB_CONFIG_WINDOW_HEIGHT) {
        client->box.height = monitor->box.height + event->height;
    }
    if (client->box.x + client->box.width > monitor->box.x + monitor->box.width) {
        client->box.x = monitor->box.x + (monitor->box.width / 2 - client_width(client) / 2);
    }
    if (client->box.x + client->box.height > monitor->box.x + monitor->box.height) {
        client->box.x = monitor->box.x + (monitor->box.height / 2 - client_height(client) / 2);
    }
    if (event->value_mask & (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y) &&
        !(event->value_mask & (XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT))) {
        client_configure(client);
    }
    if (client->tags & monitor->enabled_tags) {
        client_move_resize(client, client->box.x, client->box.y, client->box.width,
                           client->box.height);
    }
}

void handle_destroy_notify(xcb_destroy_notify_event_t *event)
{
    struct client *client = get_client_by_win(event->window);
    if (client == NULL) {
        return;
    }
    struct monitor *monitor = client->monitor;
    monitor_remove_client(monitor, client);
    free(client);
    monitor->layouts[monitor->current_layout_idx].arrange(monitor);
}

void handle_enter_notify(xcb_enter_notify_event_t *event) {}
void handle_focus_in(xcb_focus_in_event_t *event) {}
void handle_mapping_notify(xcb_mapping_notify_event_t *event) {}

void handle_map_request(xcb_map_request_event_t *event)
{
    xcb_get_window_attributes_reply_t *window_attributes_reply = xcb_get_window_attributes_reply(
        global_xconnection, xcb_get_window_attributes(global_xconnection, event->window), NULL);
    if (window_attributes_reply == NULL || window_attributes_reply->override_redirect != 0 ||
        get_client_by_win(event->window) != NULL) {
        goto WINDOW_ATTRIBUTES_REPLY_FREE;
    }

    struct monitor *monitor = global_focused_monitor;
    uint8_t tags = global_focused_monitor->enabled_tags;

    xcb_window_t transient = XCB_NONE;
    xcb_icccm_get_wm_transient_for_reply(
        global_xconnection, xcb_icccm_get_wm_transient_for(global_xconnection, event->window),
        &transient, NULL);
    if (transient != XCB_NONE) {
        struct client *client = get_client_by_win(transient);
        monitor = client->monitor;
        tags = client->tags;
    }

    xcb_get_geometry_reply_t *geometry_reply = xcb_get_geometry_reply(
        global_xconnection, xcb_get_geometry(global_xconnection, event->window), NULL);
    if (geometry_reply == NULL) {
        goto GEOMETRY_REPLY_FREE;
    }

    int16_t x = geometry_reply->x;
    uint16_t width = geometry_reply->width;
    if (x + width + 2 * geometry_reply->border_width > global_focused_monitor->box.x +
                                                           global_focused_monitor->box.width +
                                                           2 * global_client_border_width) {
        x = global_focused_monitor->box.x;
        width = global_focused_monitor->box.width;
    }

    int16_t y = geometry_reply->y;
    uint16_t height = geometry_reply->height;
    if (y + width + 2 * geometry_reply->border_width > global_focused_monitor->box.y +
                                                           global_focused_monitor->box.height +
                                                           2 * global_client_border_width) {
        y = global_focused_monitor->box.y;
        height = global_focused_monitor->box.height;
    }

    struct client *new_client = client_create(monitor, event->window, x, y, width, height,
                                              global_client_border_width, tags);
    if (new_client == NULL) {
        goto GEOMETRY_REPLY_FREE;
    }

    monitor_append_client(monitor, new_client);
    monitor->layouts[monitor->current_layout_idx].arrange(monitor);
    xcb_map_window(global_xconnection, event->window);

GEOMETRY_REPLY_FREE:
    free(geometry_reply);
WINDOW_ATTRIBUTES_REPLY_FREE:
    free(window_attributes_reply);
}

static inline uint32_t get_intersect_area_size(struct box a, struct box b)
{
    return max(0, (min(a.x + a.width, b.x + b.width) - max(a.x, b.x))) *
           max(0, (min(a.y + a.height, b.x + b.height) - max(a.y, b.y)));
}

static inline struct monitor *get_monitor_by_box(struct box box)
{
    struct monitor *result = NULL;
    uint32_t max_intersect_area_size = 0;
    for (struct list_node *cursor = global_monitors; cursor != NULL; cursor = cursor->next) {
        struct monitor *monitor = container_of(cursor, struct monitor, list_node);
        uint32_t intersect_area_size = get_intersect_area_size(box, monitor->box);
        if (intersect_area_size <= max_intersect_area_size) {
            continue;
        }
        max_intersect_area_size = intersect_area_size;
        result = monitor;
    }
    return result;
}

void monitor_focus(struct monitor *const monitor)
{
    if (global_focused_monitor->focused_client != NULL) {
        client_unfocus(global_focused_monitor->focused_client);
    }
    global_focused_monitor = monitor;
    if (global_focused_monitor->focused_client != NULL) {
        client_focus(global_focused_monitor->focused_client);
    }
}

void handle_motion_notify(xcb_motion_notify_event_t *event)
{
    if (event->root != global_screen->root) {
        return;
    }
    struct box mouse_box = {event->root_x, event->root_y, 1, 1};
    struct monitor *monitor = get_monitor_by_box(mouse_box);
    if (monitor == NULL || monitor == global_focused_monitor) {
        return;
    }
    monitor_focus(monitor);
}

void handle_property_notify(xcb_property_notify_event_t *event) {}
void handle_unmap_notify(xcb_unmap_notify_event_t *event) {}

void handle_event(xcb_generic_event_t *event)
{
    switch (XCB_EVENT_RESPONSE_TYPE(event)) {
    case XCB_BUTTON_PRESS:
        handle_button_press((xcb_button_press_event_t *)event);
        break;
    case XCB_CLIENT_MESSAGE:
        handle_client_message((xcb_client_message_event_t *)event);
        break;
    case XCB_CONFIGURE_NOTIFY:
        handle_configure_notify((xcb_configure_notify_event_t *)event);
        break;
    case XCB_CONFIGURE_REQUEST:
        handle_configure_request((xcb_configure_request_event_t *)event);
        break;
    case XCB_DESTROY_NOTIFY:
        handle_destroy_notify((xcb_destroy_notify_event_t *)event);
        break;
    case XCB_ENTER_NOTIFY:
        handle_enter_notify((xcb_enter_notify_event_t *)event);
        break;
    case XCB_FOCUS_IN:
        handle_focus_in((xcb_focus_in_event_t *)event);
        break;
    case XCB_MAPPING_NOTIFY:
        handle_mapping_notify((xcb_mapping_notify_event_t *)event);
        break;
    case XCB_MAP_REQUEST:
        handle_map_request((xcb_map_request_event_t *)event);
        break;
    case XCB_MOTION_NOTIFY:
        handle_motion_notify((xcb_motion_notify_event_t *)event);
        break;
    case XCB_PROPERTY_NOTIFY:
        handle_property_notify((xcb_property_notify_event_t *)event);
        break;
    case XCB_UNMAP_NOTIFY:
        handle_unmap_notify((xcb_unmap_notify_event_t *)event);
        break;
    }
}

int main(int argc, char *argv[])
{
    x11_init();

    while (true) {
        xcb_generic_event_t *event = xcb_poll_for_event(global_xconnection);
        if (event == NULL) {
            continue;
        }
        handle_event(event);
        free(event);
    }

    xcb_disconnect(global_xconnection);

    return 0;
}
