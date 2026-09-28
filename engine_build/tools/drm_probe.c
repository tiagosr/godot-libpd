/* drm_probe.c — minimal DRM KMS probe, no libdrm dependency.
 * Answers: does this device expose DRM-KMS (connectors/crtcs/encoders),
 * and if so what modes does the connector have?
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>

typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int32_t __s32;

struct drm_mode_res {
    __u32 count_connectors;
    __u32 connectors[64];
    __u32 count_encoders;
    __u32 encoders[64];
    __u32 count_crtcs;
    __u32 crtcs[16];
};
#define DRM_IOCTL_MODE_GETRESOURCES _IOR('R', 0x00, struct drm_mode_res)

struct drm_mode_modeinfo {
    __u16 hdisplay, hsstart, hsestart, htotal;
    __u16 vdisplay, vsstart, vsestart, vtotal;
    __u32 clock;
    unsigned char flags;
    unsigned char type;
    char name[32];
};

struct drm_mode_connector {
    __u32 connector_id;
    __u32 encoder_id;
    __u32 count_modes;
    __u32 count_props;
    __u32 count_environments;
    __u64 modes_ptr;      /* in: pointer to drm_mode_modeinfo[] */
    __u64 props_ptr;      /* in: pointer to __u32[] */
    __u64 prop_values_ptr;/* in: pointer to __u64[] */
    __u64 encoder_mask_ptr;
    int connector_type;
    int connector_type_id;
    __u32 subconnector_type;
    int mmWidth, mmHeight;
    int connection;
    __u32 count_path_blobs;
    __u32 subpixel;
    int stereo_modes;
    __u32 tile;
    __u32 display_info;
    __u32 non_desktop;
    __u32 display_early_suspended;
};
#define DRM_IOCTL_MODE_GETCONNECTOR _IOWR('R', 0x08, struct drm_mode_connector)

static const char *conn_name(int t) {
    switch (t) {
    case 1: return "VGA"; case 2: return "DVI"; case 3: return "LVDS";
    case 4: return "Component"; case 5: return "DIN"; case 6: return "TV";
    case 7: return "eDP"; case 8: return "DI"; case 9: return "VFP";
    case 10: return "DSI"; case 11: return "TMDSEncoder"; case 12: return "TVP";
    case 13: return "Writeback"; case 14: return "SPI"; case 15: return "USB";
    case 16: return "HDMI-A"; case 17: return "HDMI-B"; case 18: return "MBUS";
    case 19: return "DP"; case 20: return "Virtual"; default: return "?";
    }
}

int main(int argc, char **argv) {
    const char *node = argc > 1 ? argv[1] : "/dev/dri/card0";
    int fd = open(node, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { printf("OPEN_FAIL %s: %s\n", node, strerror(errno)); return 2; }

    struct drm_mode_res r;
    memset(&r, 0, sizeof(r));
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r) < 0) {
        printf("GETRESOURCES_FAIL: %s (errno=%d)\n", strerror(errno), errno);
        printf("=> NO DRM KMS interface\n");
        close(fd);
        return 3;
    }
    printf("RESOURCES: connectors=%u encoders=%u crtcs=%u\n",
           r.count_connectors, r.count_encoders, r.count_crtcs);

    if (r.count_connectors == 0) {
        printf("=> NO CONNECTORS: KMS unusable\n");
        close(fd);
        return 4;
    }

    for (unsigned i = 0; i < r.count_connectors && i < 64; i++) {
        struct drm_mode_modeinfo *modes = calloc(64, sizeof(*modes));
        __u32 *props = calloc(128, sizeof(__u32));
        __u64 *vals = calloc(128, sizeof(__u64));
        struct drm_mode_connector c;
        memset(&c, 0, sizeof(c));
        c.connector_id = r.connectors[i];
        c.count_modes = 64;
        c.modes_ptr = (__u64)(uintptr_t)modes;
        c.count_props = 128;
        c.props_ptr = (__u64)(uintptr_t)props;
        c.prop_values_ptr = (__u64)(uintptr_t)vals;
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0) {
            printf("CONNECTOR %u: GETCONNECTOR_FAIL: %s\n", c.connector_id,
                   strerror(errno));
            continue;
        }
        printf("CONNECTOR %u: type=%s(%d) connection=%d mm=%dx%d\n",
               c.connector_id, conn_name(c.connector_type), c.connector_type,
               c.connection, c.mmWidth, c.mmHeight);
        printf("  modes: %u\n", c.count_modes);
        for (unsigned m = 0; m < c.count_modes && m < 8; m++)
            printf("    %ux%u@%uHz clock=%u type=%u\n",
                   modes[m].hdisplay, modes[m].vdisplay,
                   (modes[m].clock + 2500) / 5000, modes[m].clock, modes[m].type);
        free(modes); free(props); free(vals);
    }
    close(fd);
    return 0;
}
