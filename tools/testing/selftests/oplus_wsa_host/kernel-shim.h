// SPDX-License-Identifier: GPL-2.0-only
/* Host-only kernel API boundary model. Never part of a kernel build. */
#ifndef WSA_HOST_KERNEL_SHIM_H
#define WSA_HOST_KERNEL_SHIM_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <time.h>
#include <limits.h>

typedef uint32_t u32;
#define __init
#define __exit
#define EPROBE_DEFER 517
#define GFP_KERNEL 0
#define CAP_NET_ADMIN 12
#define GENL_ADMIN_PERM 1
#define NLA_U32 3
#define THIS_MODULE ((void *)1)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define IS_ERR_OR_NULL(p) (!(p) || (uintptr_t)(p) >= (uintptr_t)-4095)
#define MODULE_LICENSE(x)
#define MODULE_DEVICE_TABLE(t, x)
#define of_match_ptr(x) (x)

struct mutex { pthread_mutex_t native; };
#define DEFINE_MUTEX(n) struct mutex n = { .native = PTHREAD_MUTEX_INITIALIZER }
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);
struct pinctrl_state { long level; };
struct device;
struct pinctrl {
        struct device *dev;
        struct pinctrl_state high, low;
        bool live;
};
struct device {
        void *of_node;
        void *resources[8];
        size_t resource_count;
        struct pinctrl *pins;
        bool bound;
};
struct platform_device { struct device dev; void *driver_data; };
struct of_device_id { const char *compatible; };
struct platform_driver {
        int (*probe)(struct platform_device *);
        int (*remove)(struct platform_device *);
        struct { const char *name; const struct of_device_id *of_match_table; } driver;
};
void *devm_kzalloc(struct device *dev, size_t size, int flags);
struct pinctrl *devm_pinctrl_get(struct device *dev);
struct pinctrl_state *pinctrl_lookup_state(struct pinctrl *, const char *);
int pinctrl_select_state(struct pinctrl *, struct pinctrl_state *);
void platform_set_drvdata(struct platform_device *, void *);
void *platform_get_drvdata(struct platform_device *);
int platform_driver_register(struct platform_driver *);
void platform_driver_unregister(struct platform_driver *);
void udelay(unsigned int delay);
bool capable(int cap);
int kstrtol(const char *str, unsigned int base, long *value);
void model_log(struct device *, int kind, const char *, ...)
        __attribute__((format(printf, 3, 4)));
#define dev_err(dev, ...) model_log(dev, 0, __VA_ARGS__)
#define dev_err_ratelimited(dev, ...) model_log(dev, 0, __VA_ARGS__)
#define dev_info_ratelimited(dev, ...) model_log(dev, 1, __VA_ARGS__)

struct kernel_param;
struct kernel_param_ops {
        int (*set)(const char *, const struct kernel_param *);
        int (*get)(char *, const struct kernel_param *);
};
struct kernel_param { const struct kernel_param_ops *ops; void *arg; unsigned int mode; };
int param_get_long(char *, const struct kernel_param *);
#define module_param_cb(name, param_ops, valueptr, permissions) \
        static const struct kernel_param model_param = { \
                .ops = param_ops, .arg = valueptr, .mode = permissions }
struct sk_buff { int unused; };
struct nlattr { u32 value; };
struct genl_info { struct nlattr **attrs; };
struct nla_policy { int type; };
struct genl_ops { int (*doit)(struct sk_buff *, struct genl_info *); unsigned int cmd, flags; };
struct genl_family {
        const char *name;
        unsigned int hdrsize, version, maxattr;
        const struct nla_policy *policy;
        void *module;
        const struct genl_ops *ops;
        size_t n_ops;
};
u32 nla_get_u32(const struct nlattr *);
int genl_register_family(struct genl_family *);
int genl_unregister_family(const struct genl_family *);
#define module_init(fn) static int (*model_module_init)(void) = fn
#define module_exit(fn) static void (*model_module_exit)(void) = fn
#endif
