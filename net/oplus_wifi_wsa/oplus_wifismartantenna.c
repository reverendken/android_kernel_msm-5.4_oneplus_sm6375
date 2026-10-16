/******************************************************************************
 ** Copyright (C), 2022-2022, Oplus Mobile Comm Corp., Ltd
 ** File: - oplus_wifismartantenna.c
 ** Description: wifismartantenna.c (wsa)
 **
 ** Version: 1.0
 ** Date : 2020/08/14
 ** TAG: OPLUS_FEATURE_WIFI_SMARTANTENNA
 ** ------------------------------- Revision History: ----------------------------
 ** <author>                                <data>        <version>       <desc>
 ** ------------------------------------------------------------------------------
 *******************************************************************************/

#include <linux/capability.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <net/genetlink.h>

/* This N30 implementation retains the OEM's effective GPIO-only path. */
struct oplus_wsa {
        struct device *dev;
        struct pinctrl *pinctrl;
        struct pinctrl_state *high;
        struct pinctrl_state *low;
        bool level_valid;
};

/* The wire protocol has no device selector: permit one bound device only.
 * All active-pointer, pinctrl and cached-level accesses use this mutex.
 * Never hold it while registering/unregistering either framework.
 */
static DEFINE_MUTEX(wsa_lock);
static struct oplus_wsa *wsa_active;
static long oplus_wsa_gpiolevel;

enum {
        OPLUS_WSA_LOW = 0x13,
        OPLUS_WSA_HIGH = 0x14,
};

enum {
        WSA_ATTR_UNSPEC,
        WSA_ATTR_ANT_IDX,  /* u32 */
        __WSA_ATTR_MAX,
};
#define WSA_ATTR_MAX (__WSA_ATTR_MAX - 1)

enum {
        WSA_CMD_UNSPEC,
        WSA_CMD_SWITCH_ANT,
        __WSA_CMD_MAX,
};

static const struct nla_policy wsa_cmd_policy[WSA_ATTR_MAX + 1] = {
        [WSA_ATTR_ANT_IDX] = { .type = NLA_U32 },
};

/* Last successful requested level, not an electrical readback. A failed
 * selection may partially affect hardware, so invalidate rather than invent
 * rollback or claim the old level is still effective.
 */
static int oplus_wsa_set_level(long level)
{
        struct oplus_wsa *wsa;
        int ret;

        if (level != 0 && level != 1)
                return -EINVAL;

        mutex_lock(&wsa_lock);
        wsa = wsa_active;
        if (!wsa) {
                ret = -ENODEV;
                goto out;
        }

        ret = pinctrl_select_state(wsa->pinctrl, level ? wsa->high : wsa->low);
        if (ret) {
                if (wsa->level_valid)
                        dev_err_ratelimited(wsa->dev,
                                "antenna selection failed, level %ld: %d\n",
                                level, ret);
                wsa->level_valid = false;
                goto out;
        }

        /* Preserve the OEM runtime switch-settling delay. */
        udelay(1);
        if (!wsa->level_valid)
                dev_info_ratelimited(wsa->dev, "antenna control recovered\n");
        oplus_wsa_gpiolevel = level;
        wsa->level_valid = true;
out:
        mutex_unlock(&wsa_lock);
        return ret;
}

static int wsa_doit(struct sk_buff *skb, struct genl_info *info)
{
        (void)skb;
        if (!info->attrs[WSA_ATTR_ANT_IDX])
                return -EINVAL;

        switch (nla_get_u32(info->attrs[WSA_ATTR_ANT_IDX])) {
        case OPLUS_WSA_HIGH:
                return oplus_wsa_set_level(1);
        case OPLUS_WSA_LOW:
                return oplus_wsa_set_level(0);
        default:
                return -EINVAL;
        }
}

static const struct genl_ops wsa_ops[] = {
        {
                .doit = wsa_doit,
                .cmd = WSA_CMD_SWITCH_ANT,
                .flags = GENL_ADMIN_PERM,
        },
};

static struct genl_family wsa_family = {
        .name = "WSA_GEN_NL",
        .hdrsize = 0,
        .version = 1,
        .maxattr = WSA_ATTR_MAX,
        .policy = wsa_cmd_policy,
        .module = THIS_MODULE,
        .ops = wsa_ops,
        .n_ops = ARRAY_SIZE(wsa_ops),
};

static int oplus_wsa_sysmodule_ops_set(const char *kmessage,
                                     const struct kernel_param *kp)
{
        long level;
        int ret;

        (void)kp;
        if (!capable(CAP_NET_ADMIN))
                return -EPERM;
        if (!kmessage)
                return -EINVAL;

        ret = kstrtol(kmessage, 10, &level);
        if (ret)
                return ret;
        return oplus_wsa_set_level(level);
}

static int oplus_wsa_sysmodule_ops_get(char *buffer,
                                     const struct kernel_param *kp)
{
        int ret;

        mutex_lock(&wsa_lock);
        if (!wsa_active)
                ret = -ENODEV;
        else if (!wsa_active->level_valid)
                ret = -EIO;
        else
                ret = param_get_long(buffer, kp);
        mutex_unlock(&wsa_lock);
        return ret;
}

static const struct kernel_param_ops oplus_wsa_sysmodule_ops = {
        .set = oplus_wsa_sysmodule_ops_set,
        .get = oplus_wsa_sysmodule_ops_get,
};

module_param_cb(oplus_wsa, &oplus_wsa_sysmodule_ops, &oplus_wsa_gpiolevel,
                S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

static int oplus_wsa_probe(struct platform_device *pdev)
{
        struct device *dev = &pdev->dev;
        struct oplus_wsa *wsa;
        int ret;

        if (!dev->of_node)
                return -ENODEV;

        mutex_lock(&wsa_lock);
        if (wsa_active) {
                ret = -EBUSY;
                goto out;
        }
        wsa = devm_kzalloc(dev, sizeof(*wsa), GFP_KERNEL);
        if (!wsa) {
                ret = -ENOMEM;
                goto out;
        }
        wsa->dev = dev;
        wsa->pinctrl = devm_pinctrl_get(dev);
        if (IS_ERR_OR_NULL(wsa->pinctrl)) {
                ret = wsa->pinctrl ? PTR_ERR(wsa->pinctrl) : -ENODEV;
                goto out;
        }
        wsa->high = pinctrl_lookup_state(wsa->pinctrl, "cnss_wlan_wsa_high");
        if (IS_ERR_OR_NULL(wsa->high)) {
                ret = wsa->high ? PTR_ERR(wsa->high) : -EINVAL;
                goto out;
        }
        wsa->low = pinctrl_lookup_state(wsa->pinctrl, "cnss_wlan_wsa_low");
        if (IS_ERR_OR_NULL(wsa->low)) {
                ret = wsa->low ? PTR_ERR(wsa->low) : -EINVAL;
                goto out;
        }

        /* Preserve low/primary at probe. Publish only after it succeeds. */
        ret = pinctrl_select_state(wsa->pinctrl, wsa->low);
        if (ret)
                goto out;
        oplus_wsa_gpiolevel = 0;
        wsa->level_valid = true;
        platform_set_drvdata(pdev, wsa);
        wsa_active = wsa;
out:
        if (ret && ret != -EPROBE_DEFER)
                dev_err(dev, "antenna probe failed: %d\n", ret);
        mutex_unlock(&wsa_lock);
        return ret;
}

static int oplus_wsa_remove(struct platform_device *pdev)
{
        mutex_lock(&wsa_lock);
        if (wsa_active == platform_get_drvdata(pdev))
                wsa_active = NULL;
        platform_set_drvdata(pdev, NULL);
        mutex_unlock(&wsa_lock);

        /* Driver core releases devm resources after this callback. Any
         * in-flight selection completed before the pointer was cleared;
         * subsequent commands/getters see no active device.
         */
        return 0;
}

static const struct of_device_id oplus_wsa_dt_ids[] = {
        { .compatible = "oplus,wlan-wsa" },
        {},
};
MODULE_DEVICE_TABLE(of, oplus_wsa_dt_ids);

static struct platform_driver oplus_wsa_driver = {
        .probe = oplus_wsa_probe,
        .remove = oplus_wsa_remove,
        .driver = {
                .name = "oplus_wsa",
                .of_match_table = of_match_ptr(oplus_wsa_dt_ids),
        },
};

static int __init oplus_wsa_init(void)
{
        int ret;

        ret = platform_driver_register(&oplus_wsa_driver);
        if (ret)
                return ret;
        ret = genl_register_family(&wsa_family);
        if (ret)
                platform_driver_unregister(&oplus_wsa_driver);
        return ret;
}

static void __exit oplus_wsa_fini(void)
{
        genl_unregister_family(&wsa_family);
        platform_driver_unregister(&oplus_wsa_driver);
}

module_init(oplus_wsa_init);
module_exit(oplus_wsa_fini);
MODULE_LICENSE("Dual BSD/GPL");
