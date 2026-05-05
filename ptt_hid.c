/* SPDX-License-Identifier: GPL-3.0-or-later */

#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdbool.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef EVIOCGRAB
#define EVIOCGRAB _IOW('E', 0x90, int)
#endif

#define DEFAULT_PTT_SOCKET "/tmp/udpptt.sock"
#define DEFAULT_VENDOR  0x047f
#define DEFAULT_PRODUCT 0xc058
#define DEFAULT_KEY_CODE KEY_VOLUMEUP
#define INPUT_SCAN_MAX 128
#define NAME_MAX_LEN 256
#define DEFAULT_AUTO_RELEASE_SEC 60
#define DEFAULT_REARM_MS 800

static volatile sig_atomic_t g_running = 1;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static int send_command(const char *socket_path, const char *cmd) {
    int fd = -1;
    struct sockaddr_un addr;
    ssize_t n;

    if (!socket_path || !socket_path[0] || !cmd || !cmd[0]) {
        fprintf(stderr, "invalid socket path or command\n");
        return -1;
    }

    if (strlen(socket_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "socket path too long: %s\n", socket_path);
        return -1;
    }

    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket(AF_UNIX)");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

    n = sendto(fd, cmd, strlen(cmd), 0, (struct sockaddr *)&addr, sizeof(addr));
    if (n < 0) {
        perror("sendto(AF_UNIX)");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static int parse_int_range(const char *s, int min_v, int max_v, int *out) {
    char *endp = NULL;
    long v;

    if (!s || !s[0] || !out) return -1;
    errno = 0;
    v = strtol(s, &endp, 10);
    if (errno || !endp || *endp != '\0' || v < min_v || v > max_v) return -1;
    *out = (int)v;
    return 0;
}

static int parse_u16_auto(const char *s, unsigned int *out) {
    char *endp = NULL;
    unsigned long v;

    if (!s || !s[0] || !out) return -1;
    errno = 0;
    v = strtoul(s, &endp, 0);
    if (errno || !endp || *endp != '\0' || v > 0xffffUL) return -1;
    *out = (unsigned int)v;
    return 0;
}

static int parse_key_code(const char *s, unsigned int *out) {
    unsigned int v;

    if (!s || !s[0] || !out) return -1;

    if (strcasecmp(s, "volumeup") == 0 || strcasecmp(s, "volup") == 0 ||
        strcasecmp(s, "KEY_VOLUMEUP") == 0) {
        *out = KEY_VOLUMEUP;
        return 0;
    }

    if (strcasecmp(s, "volumedown") == 0 || strcasecmp(s, "voldown") == 0 ||
        strcasecmp(s, "KEY_VOLUMEDOWN") == 0) {
        *out = KEY_VOLUMEDOWN;
        return 0;
    }

    if (strcasecmp(s, "micmute") == 0 || strcasecmp(s, "KEY_MICMUTE") == 0) {
        *out = KEY_MICMUTE;
        return 0;
    }

    if (parse_u16_auto(s, &v) == 0) {
        *out = v;
        return 0;
    }

    return -1;
}

static const char *key_name(unsigned int code) {
    switch (code) {
    case KEY_VOLUMEUP: return "KEY_VOLUMEUP";
    case KEY_VOLUMEDOWN: return "KEY_VOLUMEDOWN";
    case KEY_MICMUTE: return "KEY_MICMUTE";
    default: return "KEY_UNKNOWN_OR_NUMERIC";
    }
}

static bool test_bit(unsigned int bit, const unsigned long *array, size_t array_longs) {
    unsigned int idx = bit / (8U * sizeof(unsigned long));
    unsigned int off = bit % (8U * sizeof(unsigned long));

    if (idx >= array_longs) return false;
    return !!(array[idx] & (1UL << off));
}

static int device_has_key(int fd, unsigned int key_code) {
    unsigned long key_bits[(KEY_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))];

    memset(key_bits, 0, sizeof(key_bits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) {
        return 0;
    }

    return test_bit(key_code, key_bits, sizeof(key_bits) / sizeof(key_bits[0])) ? 1 : 0;
}

static int get_input_id(int fd, struct input_id *id) {
    memset(id, 0, sizeof(*id));
    if (ioctl(fd, EVIOCGID, id) < 0) return -1;
    return 0;
}

static int get_device_name(int fd, char *name, size_t name_sz) {
    if (!name || name_sz == 0) return -1;
    memset(name, 0, name_sz);
    if (ioctl(fd, EVIOCGNAME(name_sz - 1), name) < 0) return -1;
    name[name_sz - 1] = '\0';
    return 0;
}

static int open_matching_device(unsigned int vendor, unsigned int product, unsigned int key_code,
                                char *chosen_path, size_t chosen_path_sz) {
    for (int i = 0; i < INPUT_SCAN_MAX; ++i) {
        char path[64];
        int fd;
        struct input_id id;

        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        if (get_input_id(fd, &id) == 0 &&
            id.bustype == BUS_USB &&
            id.vendor == vendor &&
            id.product == product &&
            device_has_key(fd, key_code)) {
            if (chosen_path && chosen_path_sz > 0) {
                snprintf(chosen_path, chosen_path_sz, "%s", path);
            }
            return fd;
        }

        close(fd);
    }

    return -1;
}

static int list_matching_devices(unsigned int vendor, unsigned int product) {
    int count = 0;

    for (int i = 0; i < INPUT_SCAN_MAX; ++i) {
        char path[64];
        char name[NAME_MAX_LEN];
        int fd;
        struct input_id id;

        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        if (get_input_id(fd, &id) == 0 && id.bustype == BUS_USB && id.vendor == vendor && id.product == product) {
            int has_up = device_has_key(fd, KEY_VOLUMEUP);
            int has_down = device_has_key(fd, KEY_VOLUMEDOWN);
            int has_micmute = device_has_key(fd, KEY_MICMUTE);
            if (get_device_name(fd, name, sizeof(name)) != 0) snprintf(name, sizeof(name), "?");
            printf("%s: %s bus=0x%04x vendor=0x%04x product=0x%04x keys:%s%s%s\n",
                   path, name, id.bustype, id.vendor, id.product,
                   has_up ? " KEY_VOLUMEUP" : "",
                   has_down ? " KEY_VOLUMEDOWN" : "",
                   has_micmute ? " KEY_MICMUTE" : "");
            count++;
        }

        close(fd);
    }

    if (count == 0) {
        fprintf(stderr, "no matching USB input devices found for vendor=0x%04x product=0x%04x\n", vendor, product);
        return 1;
    }

    return 0;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [options]\n"
            "\n"
            "Options:\n"
            "  --device PATH         Use a specific /dev/input/eventX or /dev/input/by-id/... device\n"
            "  --socket PATH         ptt_client control socket, default: %s\n"
            "  --key KEY             PTT key: volumeup, volumedown, micmute, or numeric code. Default: volumeup\n"
            "  --vendor HEX          USB vendor id for auto-detect. Default: 0x%04x\n"
            "  --product HEX         USB product id for auto-detect. Default: 0x%04x\n"
            "  --list                List matching Plantronics input devices and exit\n"
            "  --auto-release-sec SEC\n"
            "                         Auto-send UP this many seconds after PTT DOWN. Default: %d\n"
            "                         Use 0 to disable automatic release.\n"
            "  --rearm-ms MS        Quiet time after a press before another press can toggle. Default: %d\n"
            "  --no-grab             Do not EVIOCGRAB the input device; useful only for debugging\n"
            "  --quiet               Reduce log output\n"
            "  --help                Show this help\n"
            "\n"
            "Examples:\n"
            "  %s --socket /tmp/udpptt.sock\n"
            "  %s --device /dev/input/by-id/usb-Plantronics_...-event-if00 --key volumeup\n",
            argv0, DEFAULT_PTT_SOCKET, DEFAULT_VENDOR, DEFAULT_PRODUCT, DEFAULT_AUTO_RELEASE_SEC, DEFAULT_REARM_MS, argv0, argv0);
}

int main(int argc, char **argv) {
    const char *device_path = NULL;
    const char *socket_path = DEFAULT_PTT_SOCKET;
    unsigned int vendor = DEFAULT_VENDOR;
    unsigned int product = DEFAULT_PRODUCT;
    unsigned int key_code = DEFAULT_KEY_CODE;
    int do_list = 0;
    int do_grab = 1;
    int quiet = 0;
    int auto_release_sec = DEFAULT_AUTO_RELEASE_SEC;
    int rearm_ms = DEFAULT_REARM_MS;
    int fd = -1;
    char auto_path[64] = {0};
    char name[NAME_MAX_LEN];
    struct input_id id;
    int ptt_down = 0;
    long long last_pulse_ms = 0;
    int armed = 1;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--device") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for --device\n"); return 1; }
            device_path = argv[++i];
        } else if (strcmp(argv[i], "--socket") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for --socket\n"); return 1; }
            socket_path = argv[++i];
        } else if (strcmp(argv[i], "--key") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for --key\n"); return 1; }
            if (parse_key_code(argv[++i], &key_code) != 0) {
                fprintf(stderr, "invalid key: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--vendor") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for --vendor\n"); return 1; }
            if (parse_u16_auto(argv[++i], &vendor) != 0) { fprintf(stderr, "invalid vendor id: %s\n", argv[i]); return 1; }
        } else if (strcmp(argv[i], "--product") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for --product\n"); return 1; }
            if (parse_u16_auto(argv[++i], &product) != 0) { fprintf(stderr, "invalid product id: %s\n", argv[i]); return 1; }
        } else if (strcmp(argv[i], "--auto-release-sec") == 0 || strcmp(argv[i], "--auto-release-seconds") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for --auto-release-sec\n"); return 1; }
            if (parse_int_range(argv[++i], 0, 86400, &auto_release_sec) != 0) {
                fprintf(stderr, "invalid value for --auto-release-sec: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--rearm-ms") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for --rearm-ms\n"); return 1; }
            if (parse_int_range(argv[++i], 1, 60000, &rearm_ms) != 0) {
                fprintf(stderr, "invalid value for --rearm-ms: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--release-delay-ms") == 0) {
            int legacy_ms;
            if (i + 1 >= argc) { fprintf(stderr, "missing value for --release-delay-ms\n"); return 1; }
            if (parse_int_range(argv[++i], 0, 86400000, &legacy_ms) != 0) {
                fprintf(stderr, "invalid value for --release-delay-ms: %s\n", argv[i]);
                return 1;
            }
            auto_release_sec = (legacy_ms + 999) / 1000;
            fprintf(stderr, "warning: --release-delay-ms is deprecated; use --auto-release-sec instead\n");
        } else if (strcmp(argv[i], "--list") == 0) {
            do_list = 1;
        } else if (strcmp(argv[i], "--no-grab") == 0) {
            do_grab = 0;
        } else if (strcmp(argv[i], "--quiet") == 0) {
            quiet = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (do_list) return list_matching_devices(vendor, product);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (device_path) {
        fd = open(device_path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            perror(device_path);
            return 1;
        }
    } else {
        fd = open_matching_device(vendor, product, key_code, auto_path, sizeof(auto_path));
        if (fd < 0) {
            fprintf(stderr, "failed to auto-detect matching HID input device for vendor=0x%04x product=0x%04x key=%s(%u)\n",
                    vendor, product, key_name(key_code), key_code);
            fprintf(stderr, "try: %s --list\n", argv[0]);
            return 1;
        }
        device_path = auto_path;
    }

    if (get_device_name(fd, name, sizeof(name)) != 0) snprintf(name, sizeof(name), "?");
    if (get_input_id(fd, &id) != 0) memset(&id, 0, sizeof(id));

    if (!device_has_key(fd, key_code)) {
        fprintf(stderr, "%s does not advertise %s(%u)\n", device_path, key_name(key_code), key_code);
        close(fd);
        return 1;
    }

    if (do_grab) {
        int grab = 1;
        if (ioctl(fd, EVIOCGRAB, &grab) < 0) {
            perror("EVIOCGRAB");
            close(fd);
            return 1;
        }
    }

    if (!quiet) {
        printf("ptt_hid: device=%s name=\"%s\" bus=0x%04x vendor=0x%04x product=0x%04x\n",
               device_path, name, id.bustype, id.vendor, id.product);
        printf("ptt_hid: key=%s(%u) socket=%s grab=%s mode=toggle auto-release-sec=%d rearm-ms=%d\n",
               key_name(key_code), key_code, socket_path, do_grab ? "yes" : "no", auto_release_sec, rearm_ms);
        fflush(stdout);
    }

    while (g_running) {
        struct input_event ev;
        long long tnow = now_ms();
        ssize_t n;

        if (!armed && last_pulse_ms > 0 && tnow - last_pulse_ms >= rearm_ms) {
            armed = 1;
            if (!quiet) { printf("[%lld] HID toggle re-armed\n", tnow); fflush(stdout); }
        }

        if (ptt_down && auto_release_sec > 0 && last_pulse_ms > 0 &&
            tnow - last_pulse_ms >= (long long)auto_release_sec * 1000LL) {
            ptt_down = 0;
            if (!quiet) { printf("[%lld] HID PTT UP (auto release after %d sec)\n", tnow, auto_release_sec); fflush(stdout); }
            if (send_command(socket_path, "UP") != 0) {
                fprintf(stderr, "failed to send UP to %s\n", socket_path);
            }
        }

        n = read(fd, &ev, sizeof(ev));

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                usleep(10000);
                continue;
            }
            perror("read(input)");
            break;
        }

        if (n != (ssize_t)sizeof(ev)) {
            usleep(10000);
            continue;
        }

        if (ev.type != EV_KEY || ev.code != key_code) continue;

        /*
         * This headset reports volume/mute as short consumer-control pulses:
         * EV_KEY value 1 followed almost immediately by value 0, and it may
         * repeat that pulse while the physical button is held.  For PTT use we
         * treat one burst of pulses as one button press.  A press toggles PTT:
         * first press sends DOWN, next re-armed press sends UP.
         */
        if (ev.value == 0) continue;

        tnow = now_ms();
        last_pulse_ms = tnow;

        if (!armed) {
            if (!quiet) { printf("[%lld] HID pulse ignored until re-arm\n", tnow); fflush(stdout); }
            continue;
        }

        armed = 0;

        if (!ptt_down) {
            ptt_down = 1;
            if (!quiet) { printf("[%lld] HID PTT DOWN (toggle)\n", tnow); fflush(stdout); }
            if (send_command(socket_path, "DOWN") != 0) {
                fprintf(stderr, "failed to send DOWN to %s\n", socket_path);
            }
        } else {
            ptt_down = 0;
            if (!quiet) { printf("[%lld] HID PTT UP (toggle)\n", tnow); fflush(stdout); }
            if (send_command(socket_path, "UP") != 0) {
                fprintf(stderr, "failed to send UP to %s\n", socket_path);
            }
        }
    }

    if (ptt_down) {
        (void)send_command(socket_path, "UP");
    }

    if (do_grab) {
        int grab = 0;
        (void)ioctl(fd, EVIOCGRAB, &grab);
    }

    close(fd);
    return 0;
}
