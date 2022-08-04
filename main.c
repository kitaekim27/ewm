#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <xcb/randr.h>
#include <xcb/xcb.h>

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
    uint8_t enabled_tags;  // bitmap.

    float aspect_ratio_min;
    float aspect_ratio_max;

    int64_t coord_x;
    int64_t coord_y;
    int64_t coord_w;
    int64_t coord_h;

    int64_t basew, baseh, incw, inch, maxw, maxh, minw, minh;
    int64_t bw, oldbw;

    bool is_fixed;
    bool is_floating;
    bool is_fullscreen;
    bool is_urgent;
    bool should_not_focus;

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

    float master_area_fraction;
    uint8_t master_area_length;

    // size_t screen_size_x;
    // size_t screen_size_y;
    // size_t screen_size_w;
    // size_t screen_size_h;

    int16_t crtc_x;
    int16_t crtc_y;
    uint16_t crtc_width;
    uint16_t crtc_height;

    size_t screen_gap_top;
    size_t screen_gap_bottom;
    size_t screen_gap_left;
    size_t screen_gap_right;

    uint8_t enabled_tags;  // bitmap.

    struct client *clients;
    struct client *focused_client;

    struct list_node list_node;

    struct layout layout;
};

static xcb_connection_t *global_xconnection = NULL;
static list_head_t global_monitors = NULL;
static xcb_screen_t *global_screen = NULL;

struct monitor *monitor_create(xcb_randr_output_t output, int16_t crtc_x, int16_t crtc_y,
                               size_t crtc_width, size_t crtc_height)
{
    struct monitor *new_monitor = (struct monitor *)malloc(sizeof(struct monitor));
    if (new_monitor == NULL) { return NULL; }
    new_monitor->crtc_x = crtc_x;
    new_monitor->crtc_y = crtc_y;
    new_monitor->crtc_width = crtc_width;
    new_monitor->crtc_height = crtc_height;
    return new_monitor;
}

int list_add(list_head_t *head, struct list_node *node)
{
    if (head == NULL) {
        *head = node;
        node->next = *head;
        return 0;
    }
    struct list_node *cursor = NULL;
    while (cursor->next != NULL) { cursor = cursor->next; }
    cursor->next = node;
    node->next = *head;
    return 0;
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
    xcb_randr_get_output_info_cookie_t *get_output_info_cookies =
        (xcb_randr_get_output_info_cookie_t *)calloc(outputs_len,
                                                     sizeof(xcb_randr_get_output_info_cookie_t));
    if (get_output_info_cookies == NULL) { return 1; }
    for (uint64_t i = 0; i < outputs_len; ++i) {
        get_output_info_cookies[i] =
            xcb_randr_get_output_info(global_xconnection, outputs[i], XCB_CURRENT_TIME);
    }

    for (uint64_t i = 0; i < outputs_len; ++i) {
        xcb_randr_get_output_info_reply_t *output_info_reply =
            xcb_randr_get_output_info_reply(global_xconnection, get_output_info_cookies[i], NULL);
        if (output_info_reply == NULL) { continue; }

        xcb_randr_get_crtc_info_reply_t *crtc_info_reply = xcb_randr_get_crtc_info_reply(
            global_xconnection,
            xcb_randr_get_crtc_info(global_xconnection, output_info_reply->crtc, XCB_CURRENT_TIME),
            NULL);
        if (crtc_info_reply == NULL) { goto OUTPUT_INFO_REPLY_FREE; }
        if (is_unique_crtc(crtc_info_reply) == false) { goto CRTC_INFO_REPLY_FREE; }

        list_add(&global_monitors,
                 &monitor_create(outputs[i], crtc_info_reply->x, crtc_info_reply->y,
                                 crtc_info_reply->width, crtc_info_reply->height)
                      ->list_node);

    CRTC_INFO_REPLY_FREE:
        free(crtc_info_reply);
    OUTPUT_INFO_REPLY_FREE:
        free(output_info_reply);
    }

    free(get_output_info_cookies);
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
    for (uint64_t i = 0; i < screen_num; ++i) { xcb_screen_next(&iterator); }
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

int main(int argc, char *argv[])
{
    x11_init();
    xcb_disconnect(global_xconnection);
    return 0;
}
