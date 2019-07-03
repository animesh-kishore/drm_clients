#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "xf86drm.h"
#include "xf86drmMode.h"
#include "libdrm_macros.h"
#include "drm_fourcc.h"

struct test_property {
	drmModeObjectPropertiesPtr obj_prop_ptr;
	drmModePropertyPtr *prop_ptr;
};

struct test_buffer {
	struct drm_mode_create_dumb dumb_buf;
	struct drm_mode_map_dumb map_dumb_buf;
	void *buf_ptr;
	uint32_t buf_id;
	uint16_t hsize, vsize;
};

/* main data structure to store info retrieved from drm drivers */
struct test_data {
	int fd;
	drmModeResPtr res_ptr;
	drmModePlaneResPtr plane_res_ptr;

	struct test_property *crtc_prop_ptr;
	struct test_property *enc_prop_ptr;
	struct test_property *con_prop_ptr;
	struct test_property *plane_prop_ptr;

	drmModeConnectorPtr active_con;
	drmModeEncoderPtr active_enc;
	drmModeCrtcPtr active_crtc;

	struct test_buffer buffer;

	struct drm_mode_create_dumb dumb_buf1;
	struct drm_mode_map_dumb map_dumb_buf1;
	void *buf_ptr1;
	uint32_t buf_id1;

	drmModeAtomicReqPtr atomic_ptr;
};

static struct test_property *
get_properties(int fd, uint32_t *obj, int n_obj, uint32_t obj_type)
{	
	int i, j;
	struct test_property *test_prop;

	test_prop = drmMalloc(n_obj * sizeof(struct test_property));
	for (i = 0; i < n_obj; i++) {
		drmModeObjectPropertiesPtr obj_prop_ptr = NULL;
		drmModePropertyPtr *prop_ptr = NULL;

		obj_prop_ptr = drmModeObjectGetProperties(fd, obj[i], obj_type);
		if (obj_prop_ptr)
			prop_ptr = drmMalloc(obj_prop_ptr->count_props * sizeof(drmModePropertyPtr));

		for (j = 0; obj_prop_ptr && j < obj_prop_ptr->count_props; j++)
			prop_ptr[j] = drmModeGetProperty(fd, obj_prop_ptr->props[j]);

		test_prop[i].obj_prop_ptr = obj_prop_ptr;
		test_prop[i].prop_ptr = prop_ptr;
	}

	return test_prop;
}

/* Get 1st connector with a valid mode */
static drmModeConnectorPtr
get_connector(int fd, uint32_t *con_id, int con_cnt)
{
	int i;

	for (i = 0; i < con_cnt; i++) {
		drmModeConnectorPtr con_ptr = drmModeGetConnector(fd, con_id[i]);
		if (con_ptr->count_modes)
			return con_ptr;
	}

	return NULL;
}

/* Get 1st encoder out of all possible encoders for selected connector */
static drmModeEncoderPtr
get_encoder(int fd, drmModeConnectorPtr con_ptr)
{
	if (!con_ptr->count_encoders)
		return NULL;

	return drmModeGetEncoder(fd, con_ptr->encoders[0]);
}

static drmModeCrtcPtr
get_crtc(int fd, drmModeResPtr res_ptr, drmModeEncoderPtr enc_ptr)
{
	int crtc_idx = ffs(enc_ptr->possible_crtcs);

	if (!crtc_idx)
		return NULL;

	return drmModeGetCrtc(fd, res_ptr->crtcs[crtc_idx - 1]);
}

static void get_dumb_buffer(int fd, struct test_buffer *buffer)
{
	struct drm_mode_create_dumb *dumb_buf = &buffer->dumb_buf;
	struct drm_mode_map_dumb *map_dumb_buf = &buffer->map_dumb_buf;

	/* Create dumb buffer */
	memset(dumb_buf, 0, sizeof(struct drm_mode_create_dumb));
	dumb_buf->bpp = 32;
	dumb_buf->width = buffer->hsize;
	dumb_buf->height =  buffer->vsize;
	drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, dumb_buf);

	/* map dumb buffer */
	memset(map_dumb_buf, 0, sizeof(struct drm_mode_map_dumb));
	map_dumb_buf->handle = dumb_buf->handle;
	drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, map_dumb_buf);
	buffer->buf_ptr = drm_mmap(0, dumb_buf->size,
		PROT_READ | PROT_WRITE, MAP_SHARED, fd, map_dumb_buf->offset);
}

#define MAKE_RGBA(r, g, b, a) \
	((((r) >> 0) << 16) | \
	 (((g) >> 0) << 8) | \
	 (((b) >> 0) << 0) | \
	 (((a) >> 8) << 0))

static void fill_pattern(void *mem_base,
			     unsigned int width, unsigned int height,
			     unsigned int stride)
{
	unsigned int x, y;

	for (y = 0; y < height; ++y) {
		for (x = 0; x < width; ++x) {
			div_t d = div(x + y, width);
			uint32_t rgb32 = 0x00130502 * (d.quot >> 6)
				       + 0x000a1120 * (d.rem >> 6);
			uint32_t alpha = ((y < height/2) && (x < width/2)) ? 127 : 255;
			uint32_t color =
				MAKE_RGBA((rgb32 >> 16) & 0xff,
					  (rgb32 >> 8) & 0xff, rgb32 & 0xff,
					  alpha);

			((uint32_t *)mem_base)[x] = color;
		}
		mem_base += stride;
	}
}

static void get_buffer(int fd, struct test_buffer *buffer)
{
	void *planes[3] = {NULL, NULL, NULL};

	/* Create and mmap a dumb buffer */
	get_dumb_buffer(fd, buffer);

	/* Draw something in the buffer */
	fill_pattern(buffer->buf_ptr, buffer->dumb_buf.width,
		buffer->dumb_buf.height, buffer->dumb_buf.pitch);
}

int main(int argc, char *argv[])
{
	struct test_data t_data;
	int i, j;
	int fd;
	drmModeResPtr res_ptr;
	drmModePlaneResPtr plane_res_ptr;
	drmModeConnectorPtr active_con;
	drmModeEncoderPtr active_enc;
	drmModeCrtcPtr active_crtc;
	struct test_buffer *buffer;
	uint64_t cap = 0;
	uint32_t bo_handles[4] = {0, 0, 0, 0};
	uint32_t pitches[4] = {0, 0, 0, 0};
	uint32_t offsets[4] = {0, 0, 0, 0};

	/* Check if drm driver name is provided by user */
	if (argc < 2) {
		printf("missing drm driver name\n");
		return -1;
	}

	/* Open drm device node /dev/dri/cardX */
	fd = drmOpen(argv[1], NULL);
	t_data.fd = fd;

	/* Check drm driver dumb buffer capability */
	if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &cap) || !cap) {
		printf("drm driver doesn't support dumb buffer\n");
		return -1;
	}

	/* Discover crtc, encoder, connector and plane resources */
	res_ptr = drmModeGetResources(fd);
	plane_res_ptr = drmModeGetPlaneResources(fd);
	t_data.res_ptr = res_ptr;
	t_data.plane_res_ptr = plane_res_ptr;

	/* Discover crtc, encoder, connector and plane properties */
	t_data.crtc_prop_ptr = get_properties(fd, res_ptr->crtcs,
		res_ptr->count_crtcs, DRM_MODE_OBJECT_CRTC);
	t_data.enc_prop_ptr = get_properties(fd, res_ptr->encoders,
		res_ptr->count_encoders, DRM_MODE_OBJECT_ENCODER);
	t_data.con_prop_ptr = get_properties(fd, res_ptr->connectors,
		res_ptr->count_connectors, DRM_MODE_OBJECT_CONNECTOR);
	t_data.plane_prop_ptr = get_properties(fd, plane_res_ptr->planes,
		plane_res_ptr->count_planes, DRM_MODE_OBJECT_PLANE);

	/* Find a connector */
	active_con = get_connector(
		fd, res_ptr->connectors, res_ptr->count_connectors);
	if (!active_con) {
		printf("no connector with valid mode found\n");
		return -1;
	}
	t_data.active_con = active_con;

	/* Find a valid encoder */
	active_enc = get_encoder(fd, active_con);
	if (!active_enc) {
		printf("no encoder available for selected connector\n");
		return -1;
	}
	t_data.active_enc = active_enc;

	/* Find a valid crtc */
	active_crtc = get_crtc(fd, res_ptr, active_enc);
	if (!active_crtc) {
		printf("no crtc available for selected encoder\n");
		return -1;
	}
	t_data.active_crtc = active_crtc;

	/* Acquire a frame buffer */
	buffer = &t_data.buffer;
	buffer->hsize = active_con->modes[0].hdisplay;
	buffer->vsize = active_con->modes[0].vdisplay;
	get_buffer(fd, buffer);

	/* Add fb to drm */
	bo_handles[0] = buffer->dumb_buf.handle;
	pitches[0] = buffer->dumb_buf.pitch;
	offsets[0] = 0;
	drmModeAddFB2(fd, buffer->dumb_buf.width, buffer->dumb_buf.height,
		DRM_FORMAT_XRGB8888, bo_handles, pitches, offsets,
		&buffer->buf_id, 0);

	/* Set mode */
	drmModeSetCrtc(fd, active_crtc->crtc_id, buffer->buf_id, 0, 0, &active_con->connector_id, 1, &active_con->modes[0]);

	getchar();

	return 0;
}
