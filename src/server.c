/* server.c - server initialization
 *
 * Copyright (C) 2024 Dwi Asmoro Bangun <dwiaceromo@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/session.h>
#include <wlr/render/allocator.h>
#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/types/wlr_content_type_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_drm.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_security_context_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_transient_seat_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_foreign_registry.h>
#include <wlr/types/wlr_xdg_foreign_v1.h>
#include <wlr/types/wlr_xdg_foreign_v2.h>
#include <wlr/types/wlr_xdg_output_v1.h>

#include "cwc/desktop/idle.h"
#include "cwc/desktop/layer_shell.h"
#include "cwc/desktop/output.h"
#include "cwc/desktop/session_lock.h"
#include "cwc/desktop/toplevel.h"
#include "cwc/input/cursor.h"
#include "cwc/input/keyboard.h"
#include "cwc/input/manager.h"
#include "cwc/input/seat.h"
#include "cwc/luac.h"
#include "cwc/plugin.h"
#include "cwc/process.h"
#include "cwc/server.h"
#include "cwc/signal.h"
#include "cwc/util.h"
#include "private/server.h"

/* sway's implementation */
static bool is_privileged(const struct wl_global *global)
{
    // drm lease check here

    return global == server.output_manager->global
           || global == server.output_power_manager->global
           || global == server.input_method_manager->global
           || global == server.foreign_toplevel_list->global
           || global == server.foreign_toplevel_manager->global
           || global == server.wlr_data_control_manager->global
           || global == server.ext_data_control_manager->global
           || global == server.screencopy_manager->global
           || global == server.copy_capture_manager->global
           || global == server.export_dmabuf_manager->global
           || global == server.security_context_manager->global
           || global == server.gamma_control_manager->global
           || global == server.layer_shell->global
           || global == server.session_lock->manager->global
           || global == server.input->kbd_inhibit_manager->global
           || global == server.input->virtual_kbd_manager->global
           || global == server.input->virtual_pointer_manager->global
           || global == server.input->transient_seat_manager->global
           || global == server.xdg_output_manager->global;
}

static bool filter_global(const struct wl_client *client,
                          const struct wl_global *global,
                          void *data)
{
    // Restrict usage of privileged protocols to unsandboxed clients
    const struct wlr_security_context_v1_state *security_context =
        wlr_security_context_manager_v1_lookup_client(
            server.security_context_manager, client);

    if (is_privileged(global))
        return security_context == NULL;

    return true;
}

/* Since the server is global and everything depends on wayland global registry
 * this should run before everything else
 */
static int setup_wayland_core(struct cwc_server *s)
{
    struct wl_display *dpy = s->wl_display = wl_display_create();
    s->wl_event_loop                       = wl_display_get_event_loop(dpy);

    wl_display_set_global_filter(s->wl_display, filter_global, NULL);
    wl_display_set_default_max_buffer_size(dpy, 1024 * 1024);

    if (!(s->backend = wlr_backend_autocreate(s->wl_event_loop, &s->session))) {
        cwc_log(CWC_ERROR, "Failed to create wlr backend");
        return EXIT_FAILURE;
    }

    s->headless_backend = wlr_headless_backend_create(s->wl_event_loop);
    if (!s->headless_backend) {
        cwc_log(CWC_ERROR, "Failed to create headless backend");
        return EXIT_FAILURE;
    }

    wlr_multi_backend_add(s->backend, s->headless_backend);

    struct wlr_renderer *drw;
    if (!(drw = s->renderer = wlr_renderer_autocreate(s->backend))) {
        cwc_log(CWC_ERROR, "Failed to create renderer");
        return EXIT_FAILURE;
    }

    s->scene = wlr_scene_create();
    wlr_renderer_init_wl_shm(drw, dpy);

    if (wlr_renderer_get_texture_formats(drw, WLR_BUFFER_CAP_DMABUF)) {
        wlr_drm_create(dpy, drw);
        wlr_scene_set_linux_dmabuf_v1(
            s->scene, wlr_linux_dmabuf_v1_create_with_renderer(dpy, 5, drw));
    }

    int drm_fd;
    if ((drm_fd = wlr_renderer_get_drm_fd(drw)) >= 0 && drw->features.timeline
        && s->backend->features.timeline)
        wlr_linux_drm_syncobj_manager_v1_create(dpy, 1, drm_fd);

    s->allocator = wlr_allocator_autocreate(s->backend, drw);
    if (s->allocator == NULL) {
        cwc_log(CWC_ERROR, "failed to create wlr_allocator");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

/* return non zero if error */
enum server_init_return_code
server_init(struct cwc_server *s, char *config_path, char *library_path)
{
    cwc_log(CWC_INFO, "Initializing cwc server...");

    if (setup_wayland_core(s) != EXIT_SUCCESS)
        return SERVER_INIT_FAILED;

    struct wl_display *dpy = s->wl_display;
    s->compositor          = wlr_compositor_create(dpy, 6, s->renderer);

    // initialize list
    wl_list_init(&s->plugins);
    wl_list_init(&s->outputs);
    wl_list_init(&s->toplevels);
    wl_list_init(&s->containers);
    wl_list_init(&s->layer_shells);
    wl_list_init(&s->kbd_kmaps);
    wl_list_init(&s->timers);

    // initialize map so that luaC can insert something at startup
    s->main_kbd_kmap      = cwc_keybind_map_create(NULL);
    s->main_mouse_kmap    = cwc_keybind_map_create(NULL);
    s->output_state_cache = cwc_hhmap_create(8);
    s->signal_map         = cwc_hhmap_create(50);
    s->input              = cwc_input_manager_get();

    int lua_status = luaC_init();
    keybind_register_common_key();

    // wlroots plug and play
    wlr_subcompositor_create(dpy);
    wlr_data_device_manager_create(dpy);
    wlr_primary_selection_v1_device_manager_create(dpy);
    wlr_viewporter_create(dpy);
    wlr_single_pixel_buffer_manager_v1_create(dpy);
    wlr_fractional_scale_manager_v1_create(dpy, 1);
    wlr_presentation_create(dpy, s->backend, 2);
    wlr_alpha_modifier_v1_create(dpy);
    wlr_ext_output_image_capture_source_manager_v1_create(dpy, 1);

    s->content_type_manager     = wlr_content_type_manager_v1_create(dpy, 1);
    s->security_context_manager = wlr_security_context_manager_v1_create(dpy);
    s->export_dmabuf_manager    = wlr_export_dmabuf_manager_v1_create(dpy);
    s->screencopy_manager       = wlr_screencopy_manager_v1_create(dpy);
    s->copy_capture_manager =
        wlr_ext_image_copy_capture_manager_v1_create(dpy, 1);
    s->wlr_data_control_manager = wlr_data_control_manager_v1_create(dpy);
    s->ext_data_control_manager =
        wlr_ext_data_control_manager_v1_create(dpy, 1);

    s->gamma_control_manager = wlr_gamma_control_manager_v1_create(dpy);
    wlr_scene_set_gamma_control_manager_v1(s->scene, s->gamma_control_manager);

    struct wlr_xdg_foreign_registry *foreign_registry =
        wlr_xdg_foreign_registry_create(dpy);
    wlr_xdg_foreign_v1_create(dpy, foreign_registry);
    wlr_xdg_foreign_v2_create(dpy, foreign_registry);

    // root scene graph
    s->temporary_tree = wlr_scene_tree_create(&s->scene->tree);
    wlr_scene_node_set_enabled(&s->temporary_tree->node, false);

    struct wlr_scene_tree *main_scene = s->main_tree =
        wlr_scene_tree_create(&s->scene->tree);
    s->root.background   = wlr_scene_tree_create(main_scene);
    s->root.bottom       = wlr_scene_tree_create(main_scene);
    s->root.below        = wlr_scene_tree_create(main_scene);
    s->root.toplevel     = wlr_scene_tree_create(main_scene);
    s->root.above        = wlr_scene_tree_create(main_scene);
    s->root.top          = wlr_scene_tree_create(main_scene);
    s->root.overlay      = wlr_scene_tree_create(main_scene);
    s->root.session_lock = wlr_scene_tree_create(main_scene);

    // desktop
    setup_output(s);
    setup_xdg_shell(s);
    setup_decoration_manager(s);

    s->foreign_toplevel_list = wlr_ext_foreign_toplevel_list_v1_create(dpy, 1);
    s->foreign_toplevel_manager = wlr_foreign_toplevel_manager_v1_create(dpy);

    s->scene_layout =
        wlr_scene_attach_output_layout(s->scene, s->output_layout);

    cwc_idle_init(s);
    setup_cwc_session_lock(s);
    setup_layer_shell(s);
    setup_transaction(s);

    // inputs
    setup_pointer(s->input);
    setup_keyboard(s->input);
    setup_seat(s->input);
    setup_text_input(s);

    setup_ipc(s);
    setup_process(s);

    const char *socket = wl_display_add_socket_auto(dpy);
    if (!socket)
        return SERVER_INIT_FAILED;

    if (!wlr_backend_start(s->backend)) {
        cwc_log(CWC_ERROR, "Failed to start wlr backend");
        return SERVER_INIT_FAILED;
    }

    setenv("WAYLAND_DISPLAY", socket, true);
    cwc_log(CWC_INFO, "Starting Wayland compositor on WAYLAND_DISPLAY=%s",
            socket);

    if (luacheck) {
        if (lua_status) {
            return LUACHECK_ERROR;
        } else {
            return LUACHECK_OK;
        }
    }

    return SERVER_INIT_SUCCESS;
}

void server_fini(struct cwc_server *s)
{
    cwc_log(CWC_INFO, "Shutting down cwc...");
    wl_display_destroy_clients(s->wl_display);

    cwc_signal_emit_c("cwc::shutdown", NULL);

    cleanup_process(s);
    cleanup_ipc(s);

    cleanup_text_input(s);
    cleanup_seat(s->input);
    cleanup_keyboard(s->input);
    cleanup_pointer(s->input);

    cleanup_output(s);
    cleanup_xdg_shell(s);
    cleanup_decoration_manager(s);
    cleanup_layer_shell(s);

    cwc_plugin_stop_plugins(&s->plugins);
    cwc_input_manager_destroy();

    cwc_idle_fini(s);

    wlr_output_layout_destroy(s->output_layout);
    wlr_allocator_destroy(s->allocator);
    wlr_renderer_destroy(s->renderer);
    cleanup_x11_bridge(s);
    wl_display_destroy(s->wl_display);
    wlr_scene_node_destroy(&s->scene->tree.node);
}

/* xwayland satellite */

// pidfd_open wrapper
static int cwc_pidfd_open(pid_t pid, unsigned int flags)
{
    return syscall(SYS_pidfd_open, pid, flags);
}

// check if xwayland-satellite is installed
bool xwayland_satellite_exists()
{
    const char *path = "xwayland-satellite";
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        /* child */
        unsetenv("DISPLAY");
        char *const argv[] = { (char *)path, (char *)":0", (char *)"--test-listenfd-support", NULL };
        execvp(path, argv);
        _exit(127); /* exec failed */
    }

    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return false;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

// create the Unix socket for :0
int open_x11_socket(int *display)
{
    char lock_path[64];
    int fd = -1;

    for (int d = 0; d <= 32; d++) {
        snprintf(lock_path, sizeof(lock_path), "/tmp/.X%d-lock", d);

        // 1. Try to create the lock file
        int lock_fd =
            open(lock_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0444);
        if (lock_fd < 0)
            continue;

        // 2. Write PID to lock file
        char pid_str[12];
        int len = snprintf(pid_str, sizeof(pid_str), "%10d\n", getpid());
        if (write(lock_fd, pid_str, len) != len) {
            close(lock_fd);
            unlink(lock_path);
            continue;
        }
        close(lock_fd);

        // 3. Create the Unix socket
        fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            unlink(lock_path);
            return -1;
        }

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(struct sockaddr_un));
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof(addr.sun_path), "/tmp/.X11-unix/X%d", d);

        mkdir("/tmp/.X11-unix", 01777);
        unlink(addr.sun_path); // Clean up stale socket if it exists

        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0
            && listen(fd, 5) == 0) {
            *display = d; // Success!
            return fd;
        }

        close(fd);
        unlink(lock_path);
    }
    return -1;
}

// auto-restart callback
static int on_satellite_exit(int fd, uint32_t mask, void *data)
{
    struct cwc_server *server = data;
    server->satellite_pid     = 0;

    // cleanup resources
    if (server->satellite_exit_source) {
        wl_event_source_remove(server->satellite_exit_source);
        server->satellite_exit_source = NULL;
    }
    close(server->satellite_pidfd);

    // reap the process to prevent zombies
    waitpid(server->satellite_pid, NULL, WNOHANG);

    return 0;
}

static int on_x11_socket_fd(int fd, uint32_t mask, void *data)
{
    struct cwc_server *server = data;

    if (server->satellite_pid)
        return 0;

    pid_t pid = fork();
    if (pid < 0)
        return 0;

    if (pid == 0) {
        // --- CHILD ---
        fcntl(server->x11_socket_fd, F_SETFD, 0); // Allow satellite to inherit FD

        char fd_str[16], disp_str[16];
        snprintf(fd_str, sizeof(fd_str), "%d", server->x11_socket_fd);
        snprintf(disp_str, sizeof(disp_str), ":%d", server->x11_display);

        char *argv[] = {"xwayland-satellite", disp_str, "-listenfd", fd_str,
                        NULL};
        execvp("xwayland-satellite", argv);
        _exit(1);
    }

    server->satellite_pid   = pid;
    server->satellite_pidfd = cwc_pidfd_open(pid, 0);

    server->satellite_exit_source =
        wl_event_loop_add_fd(server->wl_event_loop, server->satellite_pidfd,
                             WL_EVENT_READABLE, on_satellite_exit, server);

    server->satellite_pid = pid;
    return 0;
}

// spawning Logic
void setup_xwayland_satelite_integration(struct cwc_server *server)
{
    if (!xwayland_satellite_exists()) {
        fprintf(stderr,
                "[cwc] Error: xwayland-satellite binary not found in PATH.\n");
        return;
    }

    if (server->x11_socket_fd == -1) {
        server->x11_socket_fd = open_x11_socket(&server->x11_display);
        if (server->x11_socket_fd == -1) {
            fprintf(stderr, "[cwc] Failed to find an available X11 display.\n");
            return;
        }

        char disp_env[16];
        snprintf(disp_env, sizeof(disp_env), ":%d", server->x11_display);
        setenv("DISPLAY", disp_env, 1);
        fprintf(stderr, "[cwc] X11 bridge ready on DISPLAY %s\n", disp_env);
    }

    server->x11_fd_source =
        wl_event_loop_add_fd(server->wl_event_loop, server->x11_socket_fd,
                             WL_EVENT_READABLE, on_x11_socket_fd, server);
}

void cleanup_x11_bridge(struct cwc_server *server)
{
    if (server->x11_socket_fd != -1) {
        char path[128];
        snprintf(path, sizeof(path), "/tmp/.X11-unix/X%d", server->x11_display);
        unlink(path);
        snprintf(path, sizeof(path), "/tmp/.X%d-lock", server->x11_display);
        unlink(path);

        close(server->x11_socket_fd);
        server->x11_socket_fd = -1;
    }
    if (server->satellite_pidfd != -1) {
        close(server->satellite_pidfd);
        server->satellite_pidfd = -1;
    }
}
