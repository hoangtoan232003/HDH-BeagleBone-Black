#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/cdev.h>

#define DEVICE_NAME "led"
#define CLASS_NAME "led_class"

#define GPIO1_BASE 0x4804C000
#define GPIO_SIZE  0x1000

#define GPIO_OE       0x134
#define GPIO_DATAOUT  0x13C

#define GPIO_LED_WEB     28  // GPIO1_28 = GPIO60
#define GPIO_LED_TEMP    29  // GPIO1_29 = GPIO61

static int major_number;
static struct class *led_class = NULL;
static struct device *led_device = NULL;

static void __iomem *gpio_base = NULL;

static int device_open(struct inode *, struct file *);
static int device_release(struct inode *, struct file *);
static ssize_t device_read(struct file *, char __user *, size_t, loff_t *);
static ssize_t device_write(struct file *, const char __user *, size_t, loff_t *);

static struct file_operations fops = {
    .open = device_open,
    .release = device_release,
    .read = device_read,
    .write = device_write,
};

// Hàm bật/tắt LED
static void led_set(int gpio, int state) {
    int val = ioread32(gpio_base + GPIO_DATAOUT);
    if (state)
        val |= (1 << gpio);
    else
        val &= ~(1 << gpio);
    iowrite32(val, gpio_base + GPIO_DATAOUT);
}

// Hàm đọc trạng thái LED
static int led_get(int gpio) {
    int val = ioread32(gpio_base + GPIO_DATAOUT);
    return (val & (1 << gpio)) ? 1 : 0;
}

static int __init led_init(void) {
    int val;

    printk(KERN_INFO "LED driver init\n");

    major_number = register_chrdev(0, DEVICE_NAME, &fops);
    if (major_number < 0) return major_number;

    led_class = class_create(CLASS_NAME);
    if (IS_ERR(led_class)) {
        unregister_chrdev(major_number, DEVICE_NAME);
        return PTR_ERR(led_class);
    }

    led_device = device_create(led_class, NULL, MKDEV(major_number, 0), NULL, DEVICE_NAME);
    if (IS_ERR(led_device)) {
        class_destroy(led_class);
        unregister_chrdev(major_number, DEVICE_NAME);
        return PTR_ERR(led_device);
    }

    gpio_base = ioremap(GPIO1_BASE, GPIO_SIZE);
    if (!gpio_base) {
        device_destroy(led_class, MKDEV(major_number, 0));
        class_destroy(led_class);
        unregister_chrdev(major_number, DEVICE_NAME);
        return -ENOMEM;
    }

    val = ioread32(gpio_base + GPIO_OE);
    val &= ~((1 << GPIO_LED_WEB) | (1 << GPIO_LED_TEMP)); // set output
    iowrite32(val, gpio_base + GPIO_OE);

    led_set(GPIO_LED_WEB, 0);
    led_set(GPIO_LED_TEMP, 0);

    printk(KERN_INFO "LED driver loaded successfully\n");
    return 0;
}

static void __exit led_exit(void) {
    led_set(GPIO_LED_WEB, 0);
    led_set(GPIO_LED_TEMP, 0);

    if (gpio_base)
        iounmap(gpio_base);

    device_destroy(led_class, MKDEV(major_number, 0));
    class_destroy(led_class);
    unregister_chrdev(major_number, DEVICE_NAME);

    printk(KERN_INFO "LED driver unloaded\n");
}

static int device_open(struct inode *inodep, struct file *filep) {
    return 0;
}

static int device_release(struct inode *inodep, struct file *filep) {
    return 0;
}

// Đọc trạng thái 2 LED
static ssize_t device_read(struct file *filep, char __user *buffer, size_t len, loff_t *offset) {
    char status[32];
    int len_out;

    if (*offset > 0)
        return 0;

    len_out = snprintf(status, sizeof(status), "1:%d 2:%d\n",
                       led_get(GPIO_LED_WEB), led_get(GPIO_LED_TEMP));

    if (copy_to_user(buffer, status, len_out))
        return -EFAULT;

    *offset += len_out;
    return len_out;
}

// Ghi
static ssize_t device_write(struct file *filep, const char __user *buffer, size_t len, loff_t *offset) {
    char cmd[16];
    int pin, state;

    if (len > sizeof(cmd) - 1)
        return -EINVAL;

    if (copy_from_user(cmd, buffer, len))
        return -EFAULT;

    cmd[len] = '\0';

    if (sscanf(cmd, "%d:%d", &pin, &state) == 2) {
        if ((pin == 1 || pin == 2) && (state == 0 || state == 1)) {
            int gpio = (pin == 2) ? GPIO_LED_WEB : GPIO_LED_TEMP;
            led_set(gpio, state);
            return len;
        }
    }

    printk(KERN_WARNING "Invalid LED write format. Use '1:1', '1:0',...\n");
    return -EINVAL;
}

module_init(led_init);
module_exit(led_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Toan");
MODULE_DESCRIPTION("Driver Led");