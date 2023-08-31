#include <sys/socket.h>
#include <sys/un.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pixman.h>

#include <fcft/fcft.h>

#include "image.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

static pixman_color_t fg = {0xffff, 0xffff, 0xffff, 0xffff};
static pixman_color_t bg = {0x0000, 0x0000, 0x0000, 0xffff};
static pixman_color_t mg = {0x5555, 0x5555, 0x5555, 0xffff};

static int prev_ws = -1;
static int prev_prev_ws = -1;

#define	MAX_WIDTH	200
#define	MAX_HEIGHT	120
#define	SERVER_SOCK_FILE	"/tmp/stage.sock"

struct ws_surface {
	struct zwlr_layer_surface_v1 *wlr_layer_surface;
	struct wl_surface *wl_surface;
};

struct ws_output {
	struct wl_list link;
	struct wl_output *wl_output;
	struct ws_surface *ws_surface;
};

struct ws {
	struct ws_image *image;
	struct wl_buffer *wl_buffer;
	struct wl_compositor *wl_compositor;
	struct wl_display *wl_display;
	struct wl_list ws_outputs;
	struct wl_registry *wl_registry;
	struct wl_shm *wl_shm;
	struct zwlr_layer_shell_v1 *wlr_layer_shell;
};

void
layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
    uint32_t serial, uint32_t w, uint32_t h)
{

	zwlr_layer_surface_v1_ack_configure(surface, serial);
}

void
layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{

}

const static struct zwlr_layer_surface_v1_listener
    zwlr_layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

struct ws_surface *
ws_surface_create(struct ws *app, struct wl_output *wl_output)
{
	struct ws_surface *ws_surface;
	int width, height;

	width = MAX_WIDTH;
	height = MAX_HEIGHT;

	ws_surface = calloc(1, sizeof(struct ws_surface));
	if (ws_surface == NULL) {
		printf("calloc failed");
		return (NULL);
	}

	ws_surface->wl_surface =
	    wl_compositor_create_surface(app->wl_compositor);
	if (ws_surface->wl_surface == NULL) {
		printf("wl_compositor_create_surface failed");
		return (NULL);
	}

	ws_surface->wlr_layer_surface =
	    zwlr_layer_shell_v1_get_layer_surface(app->wlr_layer_shell,
	    ws_surface->wl_surface, wl_output,
	    ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "ws");
	if (ws_surface->wlr_layer_surface == NULL) {
		printf("wlr_layer_shell_v1_get_layer_surface failed");
		return (NULL);
	}

#if 0
	zwlr_layer_surface_v1_set_margin(ws_surface->wlr_layer_surface,
	    margin_top, margin_right, margin_bottom, margin_left);
#endif

	zwlr_layer_surface_v1_set_size(ws_surface->wlr_layer_surface,
	    width, height);
	zwlr_layer_surface_v1_set_anchor(ws_surface->wlr_layer_surface, 0);
	zwlr_layer_surface_v1_add_listener(ws_surface->wlr_layer_surface,
	    &zwlr_layer_surface_listener, app);

	return (ws_surface);
}

void
ws_surface_destroy(struct ws_surface *ws_surface)
{

	zwlr_layer_surface_v1_destroy(ws_surface->wlr_layer_surface);
	wl_surface_destroy(ws_surface->wl_surface);

	free(ws_surface);
}

void
ws_output_destroy(struct ws_output *output)
{

	if (output->ws_surface != NULL)
		ws_surface_destroy(output->ws_surface);

	if (output->wl_output != NULL)
		wl_output_destroy(output->wl_output);

	free(output);
}

void
handle_global(void *data, struct wl_registry *registry, uint32_t name,
    const char *interface, uint32_t version)
{
	struct ws_output *output;
	struct ws *app;

	app = (struct ws *)data;

	if (strcmp(interface, wl_shm_interface.name) == 0)
		app->wl_shm = wl_registry_bind(registry, name,
		    &wl_shm_interface, 1);
	else if (strcmp(interface, wl_compositor_interface.name) == 0)
		app->wl_compositor = wl_registry_bind(registry, name,
		    &wl_compositor_interface, 1);
	else if (strcmp(interface, wl_output_interface.name) == 0) {

		if (version < 4) {
			printf("Unsupported version\n");
			return;
		}

		output = calloc(1, sizeof(struct ws_output));
		output->wl_output = wl_registry_bind(registry, name,
		    &wl_output_interface, version);

		wl_list_insert(&app->ws_outputs, &output->link);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0)
		app->wlr_layer_shell = wl_registry_bind(registry, name,
		    &zwlr_layer_shell_v1_interface, 1);
}

void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{

	/* No support. */
}

void
ws_flush(struct ws *app)
{
	struct ws_output *output;

	wl_list_for_each (output, &app->ws_outputs, link) {
		if (output->ws_surface != NULL) 
				continue;

		output->ws_surface = ws_surface_create(app,
		    output->wl_output);
		wl_surface_commit(output->ws_surface->wl_surface);
	}

	if (wl_display_roundtrip(app->wl_display) < 0)
		printf("wl_display_roundtrip failed");

	wl_list_for_each (output, &(app->ws_outputs), link) {
		if (output->ws_surface == NULL)
			continue;

		wl_surface_attach(output->ws_surface->wl_surface,
		    app->wl_buffer, 0, 0);
		wl_surface_damage(output->ws_surface->wl_surface, 0, 0,
		    INT32_MAX, INT32_MAX);
		wl_surface_commit(output->ws_surface->wl_surface);
	}

	if (wl_display_roundtrip(app->wl_display) < 1)
		printf("wl_display_roundtrip failed");
}

int
ws_listen_sock(struct ws *app)
{
	struct sockaddr_un addr;
	struct sockaddr_un from;
	socklen_t fromlen;
	char buf[8192];
	int len;
	int fd;
	int ws;

	fromlen = sizeof(struct sockaddr_un);

	if ((fd = socket(AF_UNIX, SOCK_DGRAM, 0)) < 0)
		perror("socket");

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, SERVER_SOCK_FILE);
	unlink(SERVER_SOCK_FILE);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		perror("bind");

	do {
		len = recvfrom(fd, buf, 8192, 0, (struct sockaddr *)&from,
		    &fromlen);
		/* printf("recvfrom: %s\n", buf); */
		ws = atoi(buf);
		if (prev_prev_ws >= 0) {
			ws_image_draw(app->image, &bg, prev_prev_ws, 50, 0);
		}
		if (prev_ws >= 0) {
			ws_image_draw(app->image, &bg, prev_ws, 100, 0);
			ws_image_draw(app->image, &mg, prev_ws, 50, 0);
		}
		ws_image_draw(app->image, &fg, ws, 100, 0);

		ws_flush(app);

		prev_prev_ws = prev_ws;
		prev_ws = ws;
	} while (len > 0);

	close(fd);

	return (0);
}

static int
ws_startup(void)
{
	struct ws_output *output;
	struct wl_shm_pool *pool;
	struct ws_image *image;
	struct ws *app;

	app = calloc(1, sizeof(struct ws));
	image = ws_image_create(MAX_WIDTH, MAX_HEIGHT);

	app->image = image;

	wl_list_init(&app->ws_outputs);
	output = calloc(1, sizeof(struct ws_output));
	output->wl_output = NULL;
	wl_list_insert(&app->ws_outputs, &output->link);

	const static struct wl_registry_listener wl_registry_listener = {
		.global = handle_global,
		.global_remove = handle_global_remove,
	};

	app->wl_display = wl_display_connect(NULL);
	if (app->wl_display == NULL) {
		printf("wl_display_connect failed");
		return (-1);
	}

	app->wl_registry = wl_display_get_registry(app->wl_display);
	if (app->wl_registry == NULL) {
		printf("wl_display_get_registry failed");
		return (-1);
	}

	wl_registry_add_listener(app->wl_registry, &wl_registry_listener, app);

	if (wl_display_roundtrip(app->wl_display) < 0) {
		printf("wl_display_roundtrip failed");
		return (-1);
	}

	if (wl_display_roundtrip(app->wl_display) < 0) {
		printf("wl_display_roundtrip failed");
		return (-1);
	}

	if (app->wlr_layer_shell == NULL) {
		printf("No layer shell available\n");
		return (-1);
	}

	if (app->wl_shm == NULL) {
		printf("No wl_shm available\n");
		return (-1);
	}

	if (app->wl_compositor == NULL) {
		printf("No wl_compositor available\n");
		return (-1);
	}

	pool = wl_shm_create_pool(app->wl_shm, app->image->shmid,
	    app->image->size_in_bytes);
	if (pool == NULL) {
		printf("wl_shm_create_pool failed");
		return (-1);
	}

	app->wl_buffer = wl_shm_pool_create_buffer(pool, 0, app->image->width,
	    app->image->height, app->image->stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	if (app->wl_buffer == NULL) {
		printf("wl_shm_pool_create_buffer failed");
		return (-1);
	}

	ws_listen_sock(app);

	zwlr_layer_shell_v1_destroy(app->wlr_layer_shell);
	wl_buffer_destroy(app->wl_buffer);
	wl_compositor_destroy(app->wl_compositor);
	wl_shm_destroy(app->wl_shm);
	wl_registry_destroy(app->wl_registry);
	wl_display_roundtrip(app->wl_display);
	wl_display_disconnect(app->wl_display);
	ws_image_destroy(app->image);
	free(app);

	return (0);
}

int
main(int argc, char **argv)
{

	fcft_init(FCFT_LOG_COLORIZE_AUTO, false, FCFT_LOG_CLASS_DEBUG);
	ws_font_init();
	ws_startup();

	return (0);
}
