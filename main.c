#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <xcb/randr.h>
#include <xcb/xcb.h>
#include <xcb/xcb_event.h>
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

struct list_node {
    struct list_node *next;
};

typedef struct list_node *list_head_t;

struct client {
    char name[256];
    uint8_t tags;

    float min_aspect_ratio;
    float max_aspect_ratio;

    int16_t max_width;
    int16_t max_height;
    int16_t min_width;
    int16_t min_height;

    int16_t base_width;
    int16_t base_height;
    int16_t width_inc;
    int16_t height_inc;

    int16_t x;
    int16_t y;
    int16_t width;
    int16_t height;
    int16_t border_width;

    bool is_fixed;
    bool is_floating;
    bool is_fullscreen;
    bool is_urgent;
    bool never_focus;

    struct monitor *monitor;
    xcb_window_t window;

    struct list_node list_node;
};

struct layout {
    char name[32];
    void (*arrange)(struct monitor *);
};

struct monitor {
    xcb_randr_output_t output;

    float main_area_fraction;
    uint8_t main_area_win_num;

    int16_t crtc_x;
    int16_t crtc_y;
    uint16_t crtc_width;
    uint16_t crtc_height;

    uint16_t gap_top;
    uint16_t gap_bottom;
    uint16_t gap_left;
    uint16_t gap_right;

    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;

    uint8_t enabled_tags;

    list_head_t clients;
    struct client *focused_client;

    struct layout layouts[1];
    uint8_t current_layout;

    struct list_node list_node;
};

static xcb_connection_t *global_xconnection = NULL;

static xcb_screen_t *global_screen = NULL;
static uint16_t global_screen_width = 0;
static uint16_t global_screen_height = 0;

static list_head_t global_monitors = NULL;
static struct monitor *global_focused_monitor = NULL;

static const int16_t global_client_border_width = 8;

static inline int16_t client_width(struct client *client)
{
    return client->width + 2 * client->border_width;
}

static inline int16_t client_height(struct client *client)
{
    return client->height + 2 * client->border_width;
}

int client_set_size_hints(struct client *client)
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

struct client *client_create(struct monitor *monitor, xcb_window_t window, int16_t x, int16_t y,
                             int16_t width, int16_t height, int16_t border_width, uint8_t tags)
{
    struct client *new_client = (struct client *)malloc(sizeof(struct client));
    if (new_client == NULL) {
        return NULL;
    }
    new_client->monitor = monitor;
    new_client->window = window;
    new_client->x = x;
    new_client->y = y;
    new_client->width = width;
    new_client->height = height;
    new_client->border_width = border_width;
    if (client_set_size_hints(new_client) != 0) {
        free(new_client);
        return NULL;
    }
    new_client->tags = tags;
    return new_client;
}

void window_move_resize(xcb_window_t window, int16_t x, int16_t y, int16_t width, int16_t height)
{
    uint32_t values[] = {x, y, width, height};
    xcb_configure_window(global_xconnection, window,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |
                             XCB_CONFIG_WINDOW_HEIGHT,
                         values);
}

void tile(struct monitor *monitor)
{
    uint16_t master_area_width = monitor->width * monitor->main_area_fraction;
    uint16_t master_area_height = monitor->height / monitor->main_area_win_num;

    uint64_t clients_idx = 0;
    struct list_node *clients_cursor = monitor->clients;
    while (clients_cursor != NULL && clients_idx <= monitor->main_area_win_num) {
        struct client *client = container_of(clients_cursor, struct client, list_node);
        window_move_resize(client->window, monitor->x,
                           monitor->y + master_area_height * clients_idx, master_area_width,
                           master_area_height);
        clients_cursor = clients_cursor->next;
        ++clients_idx;
    }
    // TODO: Manage a number of clients.
    uint64_t clients_left_num = 0;
    for (struct list_node *cursor = clients_cursor; cursor != NULL; cursor = cursor->next) {
        ++clients_left_num;
    }

    clients_idx = 0;
    uint16_t slave_area_width = monitor->width - master_area_width;
    uint16_t slave_area_height = monitor->height / clients_left_num;
    while (clients_cursor != NULL) {
        struct client *client = container_of(clients_cursor, struct client, list_node);
        window_move_resize(client->window, monitor->x + master_area_width,
                           monitor->y + slave_area_height * clients_idx, slave_area_width,
                           slave_area_height);
        clients_cursor = clients_cursor->next;
    }
}

struct monitor *monitor_create(xcb_randr_output_t output, int16_t crtc_x, int16_t crtc_y,
                               size_t crtc_width, size_t crtc_height)
{
    struct monitor *new_monitor = (struct monitor *)malloc(sizeof(struct monitor));
    if (new_monitor == NULL) {
        return NULL;
    }
    new_monitor->output = output;
    new_monitor->main_area_fraction = 0.6;
    new_monitor->main_area_win_num = 1;
    new_monitor->crtc_x = crtc_x;
    new_monitor->crtc_y = crtc_y;
    new_monitor->crtc_width = crtc_width;
    new_monitor->crtc_height = crtc_height;
    new_monitor->gap_top = 8;
    new_monitor->gap_bottom = 8;
    new_monitor->gap_left = 8;
    new_monitor->gap_right = 8;
    new_monitor->x = new_monitor->crtc_x + new_monitor->gap_left;
    new_monitor->y = new_monitor->crtc_y + new_monitor->gap_top;
    new_monitor->width = new_monitor->crtc_width - new_monitor->gap_right;
    new_monitor->height = new_monitor->crtc_height - new_monitor->gap_bottom;
    struct layout layout_tile = {.name = "tile", .arrange = tile};
    new_monitor->layouts[0] = layout_tile;
    new_monitor->current_layout = 0;
    return new_monitor;
}

int list_append(list_head_t *head, struct list_node *node)
{
    if (head == NULL) {
        *head = node;
        node->next = *head;
        return 0;
    }
    struct list_node *cursor = NULL;
    while (cursor->next != NULL) {
        cursor = cursor->next;
    }
    cursor->next = node;
    node->next = *head;
    return 0;
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

bool is_unique_crtc(xcb_randr_get_crtc_info_reply_t *crtc_info_reply)
{
    for (struct list_node *cursor = global_monitors; cursor != NULL; cursor = cursor->next) {
        struct monitor *monitor = container_of(cursor, struct monitor, list_node);
        if (monitor->crtc_width == crtc_info_reply->width &&
            monitor->crtc_height == crtc_info_reply->height &&
            monitor->crtc_x == crtc_info_reply->x && monitor->crtc_y == crtc_info_reply->y) {
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
        if (crtc_info_reply == NULL || is_unique_crtc(crtc_info_reply) == false) {
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

int x11_init(void)
{
    int32_t screen_num = 0;
    global_xconnection = xcb_connect(NULL, &screen_num);
    if (xcb_connection_has_error(global_xconnection)) {
        fprintf(stderr, "Can't connect to X Server!\n");
        return 1;
    }

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
void handle_client_message(xcb_client_message_event_t *event) {}

void handle_configure_notify(xcb_configure_notify_event_t *event)
{
    if (event->window != global_screen->root) {
        return;
    }
    global_screen_height = event->width;
    global_screen_height = event->height;
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

    if (client->is_floating == true) {
        struct monitor *monitor = client->monitor;
        if (event->value_mask & XCB_CONFIG_WINDOW_X) {
            client->x = monitor->x + event->x;
        }
        if (event->value_mask & XCB_CONFIG_WINDOW_Y) {
            client->y = monitor->y + event->y;
        }
        if (event->value_mask & XCB_CONFIG_WINDOW_WIDTH) {
            client->width = monitor->width + event->width;
        }
        if (event->value_mask & XCB_CONFIG_WINDOW_HEIGHT) {
            client->height = monitor->height + event->height;
        }
        if (client->x + client->width > monitor->x + monitor->width) {
            client->x = monitor->x + (monitor->width / 2 - client_width(client) / 2);
        }
        if (client->x + client->height > monitor->x + monitor->height) {
            client->x = monitor->x + (monitor->height / 2 - client_height(client) / 2);
        }
        if (event->value_mask & (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y) &&
            !(event->value_mask & (XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT))) {
            xcb_configure_notify_event_t notify_event;

            notify_event.response_type = XCB_CONFIGURE_NOTIFY;
            notify_event.event = client->window;
            notify_event.window = client->window;
            notify_event.above_sibling = XCB_NONE;
            notify_event.x = client->x;
            notify_event.y = client->y;
            notify_event.width = client->width;
            notify_event.height = client->height;
            notify_event.border_width = client->border_width;
            notify_event.override_redirect = false;

            xcb_send_event(global_xconnection, false, event->window,
                           XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char *)&notify_event);
        }
        if (client->tags & monitor->enabled_tags) {
            window_move_resize(client->window, client->x, client->y, client->width, client->height);
        }
        return;
    }

    xcb_configure_notify_event_t notify_event;

    notify_event.response_type = XCB_CONFIGURE_NOTIFY;
    notify_event.event = client->window;
    notify_event.window = client->window;
    notify_event.above_sibling = XCB_NONE;
    notify_event.x = client->x;
    notify_event.y = client->y;
    notify_event.width = client->width;
    notify_event.height = client->height;
    notify_event.border_width = client->border_width;
    notify_event.override_redirect = false;

    xcb_send_event(global_xconnection, false, event->window, XCB_EVENT_MASK_STRUCTURE_NOTIFY,
                   (const char *)&notify_event);
}

void handle_destroy_notify(xcb_destroy_notify_event_t *event) {}
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
    if (x + width + 2 * geometry_reply->border_width > global_focused_monitor->x +
                                                           global_focused_monitor->width +
                                                           2 * global_client_border_width) {
        x = global_focused_monitor->x;
        width = global_focused_monitor->width;
    }

    int16_t y = geometry_reply->y;
    uint16_t height = geometry_reply->height;
    if (y + width + 2 * geometry_reply->border_width > global_focused_monitor->y +
                                                           global_focused_monitor->height +
                                                           2 * global_client_border_width) {
        y = global_focused_monitor->y;
        height = global_focused_monitor->height;
    }

    struct client *new_client = client_create(monitor, event->window, x, y, width, height,
                                              global_client_border_width, tags);
    if (new_client == NULL) {
        goto GEOMETRY_REPLY_FREE;
    }

    list_append(&monitor->clients, &new_client->list_node);
    monitor->layouts[monitor->current_layout].arrange(monitor);
    xcb_map_window(global_xconnection, event->window);

GEOMETRY_REPLY_FREE:
    free(geometry_reply);
WINDOW_ATTRIBUTES_REPLY_FREE:
    free(window_attributes_reply);
}

void handle_motion_notify(xcb_motion_notify_event_t *event) {}
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
