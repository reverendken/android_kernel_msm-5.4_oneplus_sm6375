// SPDX-License-Identifier: GPL-2.0-only
/* Execute the actual candidate C through registered framework callbacks only.
 * Kernel/device APIs are the mocked system boundary. No live netlink, sysfs,
 * driver loading, radio access or real capability enforcement occurs here.
 */
#include "kernel-shim.h"
#ifndef WSA_DRIVER_SOURCE
#error "Pass the candidate snapshot path through WSA_DRIVER_SOURCE"
#endif
#include WSA_DRIVER_SOURCE

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
static struct platform_driver *registered_driver;
static struct genl_family *registered_family;
static struct platform_device device1, device2;
static bool loaded, auto_bind = true, permission = true, parameter_writer;
static int platform_error, family_error, fault, pin_error = -EPROBE_DEFER;
static int select_error, probe_result;
static int driver_registers, driver_unregisters, family_registers, family_unregisters;
static int allocations, releases, select_calls, long_gets, errors, recoveries, delays;
static int sequence, family_remove_order, driver_remove_order;
static long last_selected = -1;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static bool blocking, select_entered, release_select, remove_lock_attempted;
static int select_end_order, resource_release_order;
static _Thread_local int role;

static void wake(void) { CHECK(pthread_cond_broadcast(&condition) == 0); }
static void wait_for(bool *flag)
{
        struct timespec deadline;
        CHECK(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
        deadline.tv_sec += 8;
        while (!*flag)
                CHECK(pthread_cond_timedwait(&condition, &gate, &deadline) == 0);
}
void mutex_lock(struct mutex *m)
{
        if (role == 2 && blocking) {
                CHECK(pthread_mutex_lock(&gate) == 0);
                remove_lock_attempted = true;
                wake();
                CHECK(pthread_mutex_unlock(&gate) == 0);
        }
        CHECK(pthread_mutex_lock(&m->native) == 0);
}
void mutex_unlock(struct mutex *m) { CHECK(pthread_mutex_unlock(&m->native) == 0); }
static void *owned_alloc(struct device *dev, size_t size)
{
        CHECK(dev->resource_count < ARRAY_SIZE(dev->resources));
        void *p = calloc(1, size);
        CHECK(p != NULL);
        dev->resources[dev->resource_count++] = p;
        allocations++;
        return p;
}
static void release_resources(struct device *dev)
{
        if (dev->pins)
                dev->pins->live = false;
        while (dev->resource_count) {
                free(dev->resources[--dev->resource_count]);
                releases++;
        }
        dev->pins = NULL;
        resource_release_order = ++sequence;
}
void *devm_kzalloc(struct device *dev, size_t size, int flags)
{
        (void)flags;
        if (fault == 1) return NULL;
        return owned_alloc(dev, size);
}
struct pinctrl *devm_pinctrl_get(struct device *dev)
{
        if (fault == 2) return ERR_PTR(pin_error);
        if (fault == 3) return NULL;
        struct pinctrl *p = owned_alloc(dev, sizeof(*p));
        p->dev = dev; p->high.level = 1; p->low.level = 0; p->live = true;
        dev->pins = p;
        return p;
}
struct pinctrl_state *pinctrl_lookup_state(struct pinctrl *p, const char *name)
{
        CHECK(p->live);
        if (!strcmp(name, "cnss_wlan_wsa_high")) {
                if (fault == 4) return ERR_PTR(pin_error);
                if (fault == 5) return NULL;
                return &p->high;
        }
        CHECK(!strcmp(name, "cnss_wlan_wsa_low"));
        if (fault == 6) return ERR_PTR(pin_error);
        if (fault == 7) return NULL;
        return &p->low;
}
int pinctrl_select_state(struct pinctrl *p, struct pinctrl_state *state)
{
        CHECK(p->live);
        CHECK(state == &p->high || state == &p->low);
        select_calls++;
        if (blocking && role == 1) {
                CHECK(pthread_mutex_lock(&gate) == 0);
                select_entered = true; wake(); wait_for(&release_select);
                CHECK(pthread_mutex_unlock(&gate) == 0);
                CHECK(p->live); /* ASan catches premature devm release here too. */
                select_end_order = ++sequence;
        }
        if (select_error) return select_error;
        last_selected = state->level;
        return 0;
}
void platform_set_drvdata(struct platform_device *p, void *data) { p->driver_data = data; }
void *platform_get_drvdata(struct platform_device *p) { return p->driver_data; }
static int bind_device(struct platform_device *p)
{
        CHECK(registered_driver && !p->dev.bound);
        int ret = registered_driver->probe(p);
        if (!ret) p->dev.bound = true;
        else release_resources(&p->dev); /* Driver core's failed-probe cleanup. */
        return ret;
}
static void unbind_device(struct platform_device *p)
{
        CHECK(p->dev.bound && registered_driver);
        CHECK(registered_driver->remove(p) == 0);
        p->dev.bound = false;
        CHECK(p->driver_data == NULL);
        release_resources(&p->dev); /* Core releases devm AFTER remove returns. */
}
int platform_driver_register(struct platform_driver *driver)
{
        driver_registers++;
        if (platform_error) return platform_error;
        CHECK(!registered_driver); registered_driver = driver;
        if (auto_bind) probe_result = bind_device(&device1);
        /* Registration can succeed even when a device probe fails. */
        return 0;
}
void platform_driver_unregister(struct platform_driver *driver)
{
        CHECK(registered_driver == driver);
        if (device1.dev.bound) unbind_device(&device1);
        if (device2.dev.bound) unbind_device(&device2);
        registered_driver = NULL; driver_unregisters++;
        driver_remove_order = ++sequence;
}
int genl_register_family(struct genl_family *family)
{
        family_registers++;
        if (family_error) return family_error;
        CHECK(!registered_family); registered_family = family;
        CHECK(!strcmp(family->name, "WSA_GEN_NL"));
        CHECK(family->version == 1 && family->hdrsize == 0 && family->maxattr == 1);
        CHECK(family->n_ops == 1 && family->ops[0].cmd == 1);
        CHECK(family->ops[0].flags == GENL_ADMIN_PERM);
        CHECK(family->policy[1].type == NLA_U32);
        return 0;
}
int genl_unregister_family(const struct genl_family *family)
{
        CHECK(registered_family == family);
        registered_family = NULL; family_unregisters++;
        family_remove_order = ++sequence;
        return 0;
}
void udelay(unsigned int us) { CHECK(us == 1); delays++; }
bool capable(int cap) { CHECK(cap == CAP_NET_ADMIN); return permission; }
int kstrtol(const char *str, unsigned int base, long *value)
{
        char *end;
        errno = 0;
        long parsed = strtol(str, &end, (int)base);
        if (errno == ERANGE) return -ERANGE;
        if (end == str) return -EINVAL;
        if (*end == '\n') end++;
        if (*end) return -EINVAL;
        *value = parsed;
        return 0;
}
int param_get_long(char *buffer, const struct kernel_param *kp)
{
        long_gets++;
        return snprintf(buffer, 4096, "%ld\n", *(long *)kp->arg);
}
void model_log(struct device *dev, int kind, const char *fmt, ...)
{
        (void)dev; (void)fmt;
        if (kind) recoveries++; else errors++;
}
u32 nla_get_u32(const struct nlattr *a) { return a->value; }

/* Public module / parameter / generic-netlink boundary model. */
static int load_module(void)
{
        CHECK(!loaded);
        int ret = model_module_init();
        loaded = ret == 0;
        return ret;
}
static void unload_module(void)
{
        CHECK(loaded); model_module_exit(); loaded = false;
        CHECK(!registered_driver && !registered_family);
        CHECK(allocations == releases);
        CHECK(family_remove_order < driver_remove_order);
}
static int set_parameter(const char *s)
{
        CHECK(loaded); return model_param.ops->set(s, &model_param);
}
static int get_parameter(char *s)
{
        CHECK(loaded); return model_param.ops->get(s, &model_param);
}
static int send_command(u32 value, bool with_attr)
{
        if (!registered_family) return -ENOENT;
        /* This models framework admission, NOT real capability enforcement. */
        if (!permission) return -EPERM;
        struct nlattr a = { .value = value };
        struct nlattr *attrs[2] = { NULL, with_attr ? &a : NULL };
        struct genl_info info = { .attrs = attrs };
        struct sk_buff skb = { 0 };
        return registered_family->ops[0].doit(&skb, &info);
}
static void expect_level(long level)
{
        char buffer[4096], expected[32];
        snprintf(expected, sizeof(expected), "%ld\n", level);
        CHECK(get_parameter(buffer) == (int)strlen(expected));
        CHECK(!strcmp(buffer, expected));
        CHECK(last_selected == level);
}
static void *writer(void *unused)
{
        (void)unused; role = 1;
        CHECK((parameter_writer ? set_parameter("1") : send_command(0x14, true)) == 0);
        return NULL;
}
static void *remover(void *unused)
{
        (void)unused; role = 2; unbind_device(&device1); return NULL;
}

int main(int argc, char **argv)
{
        CHECK(argc == 2);
        device1.dev.of_node = (void *)1; device2.dev.of_node = (void *)1;
        CHECK(model_param.mode == 0644);
        const char *test = argv[1]; char buffer[4096];
        if (!strcmp(test, "registration-failure")) {
                platform_error = -EBUSY;
                CHECK(load_module() == -EBUSY);
                CHECK(!registered_driver && !registered_family && family_registers == 0);
                CHECK(allocations == releases);
        } else if (!strcmp(test, "family-failure-unwind")) {
                family_error = -EEXIST;
                CHECK(load_module() == -EEXIST);
                CHECK(!registered_driver && !registered_family);
                CHECK(driver_unregisters == 1 && allocations == releases);
                family_error = 0; auto_bind = false;
                CHECK(load_module() == 0);
                CHECK(get_parameter(buffer) == -ENODEV);
                CHECK(send_command(0x14, true) == -ENODEV);
                unload_module();
        } else if (!strcmp(test, "unbound-interfaces")) {
                auto_bind = false; CHECK(load_module() == 0);
                CHECK(get_parameter(buffer) == -ENODEV);
                CHECK(set_parameter("1") == -ENODEV);
                CHECK(send_command(0x14, true) == -ENODEV);
                CHECK(select_calls == 0); unload_module();
        } else if (!strcmp(test, "normal-control")) {
                CHECK(load_module() == 0 && probe_result == 0);
                expect_level(0); CHECK(delays == 0);
                CHECK(send_command(0x14, true) == 0); expect_level(1);
                CHECK(set_parameter("0\n") == 0); expect_level(0);
                CHECK(set_parameter("1") == 0); expect_level(1);
                CHECK(send_command(0x13, true) == 0); expect_level(0);
                CHECK(delays == 4 && long_gets == 5); unload_module();
        } else if (!strcmp(test, "rejected-inputs")) {
                CHECK(load_module() == 0); int calls = select_calls;
                CHECK(set_parameter(NULL) == -EINVAL);
                CHECK(set_parameter("bad") == -EINVAL);
                CHECK(set_parameter("2") == -EINVAL);
                CHECK(set_parameter("-1") == -EINVAL);
                CHECK(set_parameter("999999999999999999999999") == -ERANGE);
                CHECK(send_command(0x14, false) == -EINVAL);
                CHECK(send_command(0x99, true) == -EINVAL);
                permission = false;
                CHECK(set_parameter("1") == -EPERM);
                CHECK(set_parameter(NULL) == -EPERM);
                CHECK(send_command(0x14, true) == -EPERM);
                expect_level(0); /* Reading remains permitted without NET_ADMIN. */
                permission = true;
                CHECK(select_calls == calls); unload_module();
        } else if (!strcmp(test, "failed-switch-recovery")) {
                CHECK(load_module() == 0); select_error = -EIO;
                CHECK(send_command(0x14, true) == -EIO);
                CHECK(get_parameter(buffer) == -EIO);
                CHECK(errors == 1 && recoveries == 0);
                CHECK(set_parameter("0") == -EIO);
                CHECK(errors == 1); select_error = 0;
                CHECK(set_parameter("1") == 0); expect_level(1);
                CHECK(recoveries == 1); unload_module();
        } else if (!strcmp(test, "duplicate-device")) {
                CHECK(load_module() == 0); CHECK(set_parameter("1") == 0);
                int calls = select_calls, count = allocations;
                CHECK(bind_device(&device2) == -EBUSY);
                CHECK(!device2.dev.bound && !device2.driver_data);
                CHECK(select_calls == calls && allocations == count);
                expect_level(1); unload_module();
        } else if (!strcmp(test, "remove-rebind")) {
                CHECK(load_module() == 0); CHECK(set_parameter("1") == 0);
                unbind_device(&device1);
                CHECK(get_parameter(buffer) == -ENODEV);
                CHECK(set_parameter("1") == -ENODEV);
                CHECK(send_command(0x13, true) == -ENODEV);
                CHECK(allocations == releases);
                CHECK(bind_device(&device1) == 0); expect_level(0); unload_module();
        } else if (!strcmp(test, "select-remove-race") || !strcmp(test, "parameter-remove-race")) {
                parameter_writer = !strcmp(test, "parameter-remove-race");
                CHECK(load_module() == 0); blocking = true;
                pthread_t a, b;
                CHECK(pthread_create(&a, NULL, writer, NULL) == 0);
                CHECK(pthread_mutex_lock(&gate) == 0); wait_for(&select_entered);
                CHECK(pthread_mutex_unlock(&gate) == 0);
                CHECK(pthread_create(&b, NULL, remover, NULL) == 0);
                CHECK(pthread_mutex_lock(&gate) == 0); wait_for(&remove_lock_attempted);
                release_select = true; wake(); CHECK(pthread_mutex_unlock(&gate) == 0);
                CHECK(pthread_join(a, NULL) == 0 && pthread_join(b, NULL) == 0);
                CHECK(select_end_order < resource_release_order);
                CHECK(allocations == releases);
                CHECK(get_parameter(buffer) == -ENODEV);
                CHECK(send_command(0x14, true) == -ENODEV);
                unload_module();
        } else if (!strcmp(test, "repeated-lifecycle")) {
                for (int i = 0; i < 50; i++) {
                        CHECK(load_module() == 0); expect_level(0);
                        CHECK(set_parameter("1") == 0); unload_module();
                }
                CHECK(driver_registers == 50 && family_registers == 50);
                CHECK(driver_unregisters == 50 && family_unregisters == 50);
        } else if (!strcmp(test, "probe-select-failure")) {
                select_error = -EIO; CHECK(load_module() == 0);
                CHECK(probe_result == -EIO && !device1.dev.bound);
                CHECK(get_parameter(buffer) == -ENODEV && allocations == releases);
                select_error = 0; CHECK(bind_device(&device1) == 0); expect_level(0); unload_module();
        } else if (!strcmp(test, "missing-dt")) {
                device1.dev.of_node = NULL; CHECK(load_module() == 0);
                CHECK(probe_result == -ENODEV && allocations == 0);
                CHECK(get_parameter(buffer) == -ENODEV); unload_module();
        } else if (!strncmp(test, "probe-fault-", 12)) {
                fault = atoi(test + 12); CHECK(fault >= 1 && fault <= 7);
                int expected = fault == 1 ? -ENOMEM :
                        fault == 3 ? -ENODEV : (fault == 5 || fault == 7) ? -EINVAL : pin_error;
                CHECK(load_module() == 0); CHECK(probe_result == expected);
                CHECK(errors == (expected == -EPROBE_DEFER ? 0 : 1));
                CHECK(!device1.dev.bound && allocations == releases);
                CHECK(get_parameter(buffer) == -ENODEV);
                CHECK(send_command(0x14, true) == -ENODEV);
                fault = 0; CHECK(bind_device(&device1) == 0); expect_level(0); unload_module();
        } else {
                CHECK(false);
        }
        CHECK(!loaded && !registered_driver && !registered_family);
        CHECK(allocations == releases);
        printf("PASS %s\n", test);
        return 0;
}
