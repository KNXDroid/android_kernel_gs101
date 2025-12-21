#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/fps_listener.h>

/* Global variable to store the requested FPS */
int g_target_fps = 60;

/*
 * Export this symbol so other kernel drivers (like your DSI panel driver)
 * can read it if they want to actually adjust hardware clocks.
 */
EXPORT_SYMBOL(g_target_fps);

/* Variable to hold the kobject */
static struct kobject *fps_kobj;

/*
 * The "show" function: Reads the value
 * Cat path: /sys/kernel/fps_listener/rate
 */
static ssize_t rate_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%d\n", g_target_fps);
}

/*
 * The "store" function: Writes the value
 * Echo path: /sys/kernel/fps_listener/rate
 */
static ssize_t rate_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
    int ret;
    int val;

    ret = kstrtoint(buf, 10, &val);
    if (ret < 0)
        return ret;

    g_target_fps = val;

    // Optional: Print to dmesg for debugging
    // printk(KERN_INFO "Universal FPS: SurfaceFlinger requested %d Hz\n", val);

    return count;
}

/* Define the attribute */
static struct kobj_attribute fps_attribute = {
    .attr = {
        .name = "rate",
        .mode = 0666, /* Read/Write for everyone */
    },
    .show = rate_show,
    .store = rate_store,
};

/* Initialize the module */
static int __init fps_listener_init(void)
{
    int error = 0;

    // Create a folder named "fps_listener" inside /sys/kernel/
    fps_kobj = kobject_create_and_add("fps_listener", kernel_kobj);
    if (!fps_kobj)
        return -ENOMEM;

    // Create the "rate" file inside that folder
    error = sysfs_create_file(fps_kobj, &fps_attribute.attr);
    if (error) {
        kobject_put(fps_kobj);
    }

    return error;
}

/* Cleanup the module */
static void __exit fps_listener_exit(void)
{
    kobject_put(fps_kobj);
}

module_init(fps_listener_init);
module_exit(fps_listener_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("danya2271");
MODULE_DESCRIPTION("Universal Dynamic FPS Interface for SurfaceFlinger");
