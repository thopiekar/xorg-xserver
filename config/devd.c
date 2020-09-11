/*
 * Copyright © 2012 Baptiste Daroussin
 * Copyright © 2014 Robert Millan
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Author: Baptiste Daroussin <bapt@FreeBSD.org>
 */

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/un.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <unistd.h>

#include "input.h"
#include "inputstr.h"
#include "hotplug.h"
#include "config-backends.h"
#include "os.h"

#define DEVD_SOCK_PATH "/var/run/devd.pipe"

#define DEVD_EVENT_ADD		'+'
#define DEVD_EVENT_REMOVE	'-'

static int sock_devd = -1;

struct hw_type {
    const char *driver;
    int flag;
    const char *xdriver;
};

static struct hw_type hw_types[] = {
    {"ukbd", ATTR_KEYBOARD, "kbd"},
    {"atkbd", ATTR_KEYBOARD, "kbd"},
    {"ums", ATTR_POINTER, "mouse"},
    {"psm", ATTR_POINTER, "mouse"},
    {"uhid", ATTR_POINTER, "mouse"},
    {"joy", ATTR_JOYSTICK, NULL},
    {"atp", ATTR_TOUCHPAD, NULL},
    {"uep", ATTR_TOUCHSCREEN, NULL},
    {NULL, -1, NULL},
};

static bool
sysctl_exists(const char *format, ...)
{
    va_list args;
    char *name = NULL;
    size_t len;
    int ret;

    if (format == NULL)
        return false;

    va_start(args, format);
    vasprintf(&name, format, args);
    va_end(args);

    ret = sysctlbyname(name, NULL, &len, NULL, 0);

    if (ret == -1)
        len = 0;

    free(name);
    return (len > 0);
}

static char *
sysctl_get_str(const char *format, ...)
{
    va_list args;
    char *name = NULL;
    char *dest = NULL;
    size_t len;

    if (format == NULL)
        return NULL;

    va_start(args, format);
    vasprintf(&name, format, args);
    va_end(args);

    if (sysctlbyname(name, NULL, &len, NULL, 0) == 0) {
        dest = malloc(len + 1);
        if (!dest)
            goto unwind;
        if (sysctlbyname(name, dest, &len, NULL, 0) == 0)
            dest[len] = '\0';
        else {
            free(dest);
            dest = NULL;
        }
    }

 unwind:
    free(name);
    return dest;
}

static void
device_added(char *devname)
{
    char path[PATH_MAX];
    char *vendor;
    char *product = NULL;
    char *config_info = NULL;
    char *walk;
    InputOption *options = NULL;
    InputAttributes attrs = { };
    DeviceIntPtr dev = NULL;
    int i, rc;
    int fd;

    for (i = 0; hw_types[i].driver != NULL; i++) {
        if (strncmp(devname, hw_types[i].driver,
                    strlen(hw_types[i].driver)) == 0 &&
            isdigit(*(devname + strlen(hw_types[i].driver)))) {
            attrs.flags |= hw_types[i].flag;
            break;
        }
    }
    if (hw_types[i].driver == NULL) {
        LogMessageVerb(X_INFO, 10, "config/devd: ignoring device %s\n",
                       devname);
        return;
    }
    if (hw_types[i].xdriver == NULL) {
        LogMessageVerb(X_INFO, 10, "config/devd: ignoring device %s\n",
                       devname);
        return;
    }
    snprintf(path, sizeof(path), "/dev/%s", devname);

    options = input_option_new(NULL, "_source", "server/devd");
    if (!options)
        return;

    vendor =
        sysctl_get_str("dev.%s.%s.%%desc", hw_types[i].driver,
                       devname + strlen(hw_types[i].driver));
    if (vendor == NULL) {
        attrs.vendor = strdup("(unnamed)");
        attrs.product = strdup("(unnamed)");
    }
    else {
        if ((walk = strchr(vendor, ' ')) != NULL) {
            walk[0] = '\0';
            walk++;
            product = walk;
            if ((walk = strchr(product, ',')) != NULL)
                walk[0] = '\0';
        }

        attrs.vendor = strdup(vendor);
        if (product) 
            attrs.product = strdup(product);
        else
            attrs.product = strdup("(unnamed)");

        options = input_option_new(options, "name", xstrdup(attrs.product));

        free(vendor);
    }
    attrs.usb_id = NULL;
    attrs.device = strdup(path);
    options = input_option_new(options, "driver", hw_types[i].xdriver);
    if (attrs.flags & ATTR_KEYBOARD) {
        /*
         * Don't pass device option if keyboard is attached to console (open fails),
         * thus activating special logic in xf86-input-keyboard.
         */
        fd = open(path, O_RDONLY | O_NONBLOCK | O_EXCL);
        if (fd > 0) {
            close(fd);
            options = input_option_new(options, "device", xstrdup(path));
        }
    }
    else {
        options = input_option_new(options, "device", xstrdup(path));
    }

    if (asprintf(&config_info, "devd:%s", devname) == -1) {
        config_info = NULL;
        goto unwind;
    }

    if (device_is_duplicate(config_info)) {
        LogMessage(X_WARNING, "config/devd: device %s already added. "
                   "Ignoring.\n", attrs.product);
        goto unwind;
    }

    options = input_option_new(options, "config_info", config_info);
    LogMessage(X_INFO, "config/devd: adding input device %s (%s)\n",
               attrs.product, path);

    rc = NewInputDeviceRequest(options, &attrs, &dev);

    if (rc != Success)
        goto unwind;

 unwind:
    free(config_info);
    input_option_free_list(&options);

    free(attrs.usb_id);
    free(attrs.product);
    free(attrs.device);
    free(attrs.vendor);
}

static void
device_removed(char *devname)
{
    char *value;

    if (asprintf(&value, "devd:%s", devname) == -1)
        return;

    remove_devices("devd", value);

    free(value);
}

static ssize_t
socket_getline(int fd, char **out)
{
    char *buf, *newbuf;
    ssize_t ret, cap, sz = 0;
    char c;

    cap = 1024;
    buf = malloc(cap * sizeof(char));
    if (!buf)
        return -1;

    for (;;) {
        ret = read(sock_devd, &c, 1);
        if (ret < 1) {
            if (errno == EINTR)
                continue;
            free(buf);
            return -1;
        }

        if (c == '\n')
            break;

        if (sz + 1 >= cap) {
            cap *= 2;
            newbuf = realloc(buf, cap * sizeof(char));
            if (!newbuf) {
                free(buf);
                return -1;
            }
            buf = newbuf;
        }
        buf[sz] = c;
        sz++;
    }

    buf[sz] = '\0';
    if (sz >= 0)
        *out = buf;
    else
        free(buf);

    return sz;                  /* number of bytes in the line, not counting the line break */
}

static void
socket_handler(int fd, int ready, void *data)
{
    char *line = NULL;
    char *walk;

    if (socket_getline(sock_devd, &line) < 0)
        return;

    walk = strchr(line + 1, ' ');
    if (walk != NULL)
        walk[0] = '\0';

    switch (*line) {
    case DEVD_EVENT_ADD:
        device_added(line + 1);
        break;
    case DEVD_EVENT_REMOVE:
        device_removed(line + 1);
        break;
    default:
        break;
    }
    free(line);
}

int
config_devd_init(void)
{
    struct sockaddr_un devd;
    char devicename[1024];
    int i, j;

    /* first scan the sysctl to determine the hardware if needed */

    for (i = 0; hw_types[i].driver != NULL; i++) {
        for (j = 0; sysctl_exists("dev.%s.%i.%%desc", hw_types[i].driver, j);
             j++) {
            snprintf(devicename, sizeof(devicename), "%s%i", hw_types[i].driver,
                     j);
            device_added(devicename);
        }

    }
    sock_devd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_devd < 0) {
        ErrorF("config/devd: Fail opening stream socket");
        return 0;
    }

    devd.sun_family = AF_UNIX;
    strlcpy(devd.sun_path, DEVD_SOCK_PATH, sizeof(devd.sun_path));

    if (connect(sock_devd, (struct sockaddr *) &devd, sizeof(devd)) < 0) {
        close(sock_devd);
        ErrorF("config/devd: Fail to connect to devd");
        return 0;
    }

    SetNotifyFd(sock_devd, socket_handler, X_NOTIFY_READ, NULL);

    return 1;
}

void
config_devd_fini(void)
{
    if (sock_devd < 0)
        return;

    RemoveNotifyFd(sock_devd);
    close(sock_devd);
}
