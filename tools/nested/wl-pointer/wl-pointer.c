// wl-pointer <fx> <fy> [click|rclick|mclick]   Move the pointer to a fraction (0..1) of the output and optionally click a button.
// For driving a nested compositor in tests (zwlr_virtual_pointer_v1). Run with WAYLAND_DISPLAY set to the nested display.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct wl_seat *seat;
static struct zwlr_virtual_pointer_manager_v1 *manager;

static void global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version) {
  (void)data; (void)version;
  if (!strcmp(iface, "wl_seat") && !seat) seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
  else if (!strcmp(iface, "zwlr_virtual_pointer_manager_v1")) manager = wl_registry_bind(reg, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
}
static void global_remove(void *data, struct wl_registry *reg, uint32_t name) { (void)data; (void)reg; (void)name; }
static const struct wl_registry_listener listener = {global, global_remove};

static uint32_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000); }

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: wl-pointer <fx> <fy> [click|rclick|mclick|dclick|drag <tx> <ty>]\n"); return 2; }
  double fx = atof(argv[1]), fy = atof(argv[2]);
  struct wl_display *display = wl_display_connect(NULL);
  if (!display) { fprintf(stderr, "no wayland display\n"); return 1; }
  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &listener, NULL);
  wl_display_roundtrip(display);
  if (!manager) { fprintf(stderr, "compositor has no zwlr_virtual_pointer_manager_v1\n"); return 1; }
  struct zwlr_virtual_pointer_v1 *pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, seat);
  const uint32_t extent = 100000;
  zwlr_virtual_pointer_v1_motion_absolute(pointer, now_ms(), (uint32_t)(fx * extent), (uint32_t)(fy * extent), extent, extent);
  zwlr_virtual_pointer_v1_frame(pointer);
  wl_display_roundtrip(display);
  usleep(150000);
  uint32_t button = 0;
  if (argc > 3) {
    if (!strcmp(argv[3], "click")) button = 0x110;        // BTN_LEFT
    else if (!strcmp(argv[3], "rclick")) button = 0x111;  // BTN_RIGHT
    else if (!strcmp(argv[3], "mclick")) button = 0x112;  // BTN_MIDDLE
    else if (!strcmp(argv[3], "dclick")) button = 0x110;  // two clicks close together: selects a word
  }
  if (button && argc > 3 && !strcmp(argv[3], "dclick")) {
    for (int i = 0; i < 2; i++) {
      zwlr_virtual_pointer_v1_button(pointer, now_ms(), button, 1);
      zwlr_virtual_pointer_v1_frame(pointer);
      wl_display_roundtrip(display);
      usleep(40000);
      zwlr_virtual_pointer_v1_button(pointer, now_ms(), button, 0);
      zwlr_virtual_pointer_v1_frame(pointer);
      wl_display_roundtrip(display);
      usleep(60000);
    }
    button = 0;
  }
  if (argc > 5 && !strcmp(argv[3], "drag")) {
    // Press here, move to the target in a few steps, release there: selects text or draws a rectangle.
    double tx = atof(argv[4]), ty = atof(argv[5]);
    zwlr_virtual_pointer_v1_button(pointer, now_ms(), 0x110, 1);
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_roundtrip(display);
    for (int i = 1; i <= 8; i++) {
      zwlr_virtual_pointer_v1_motion_absolute(pointer, now_ms(), (uint32_t)((fx + (tx - fx) * i / 8) * extent), (uint32_t)((fy + (ty - fy) * i / 8) * extent), extent, extent);
      zwlr_virtual_pointer_v1_frame(pointer);
      wl_display_roundtrip(display);
      usleep(40000);
    }
    zwlr_virtual_pointer_v1_button(pointer, now_ms(), 0x110, 0);
    zwlr_virtual_pointer_v1_frame(pointer);
  }
  if (button) {
    zwlr_virtual_pointer_v1_button(pointer, now_ms(), button, 1);  // pressed
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_roundtrip(display);
    usleep(60000);
    zwlr_virtual_pointer_v1_button(pointer, now_ms(), button, 0);
    zwlr_virtual_pointer_v1_frame(pointer);
  }
  wl_display_roundtrip(display);
  zwlr_virtual_pointer_v1_destroy(pointer);
  wl_display_roundtrip(display);
  return 0;
}
