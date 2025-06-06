#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/jiffies.h>
#include <linux/timer.h>
#include <linux/errno.h>
#include <linux/slab.h>

/* Device config */
#define DEVICE_NAME "bh1750"
#define CLASS_NAME  "bh1750_class"

/* BH1750 I2C addr & commands */
#define BH1750_ADDR 0x23
#define CMD_POWER_DOWN   0x00
#define CMD_POWER_ON     0x01
#define CMD_RESET        0x07
#define CMD_CONT_HIGH    0x10
#define CMD_CONT_HIGH2   0x11
#define CMD_CONT_LOW     0x13
#define CMD_ONETIME_HIGH 0x20
#define CMD_ONETIME_LOW  0x23

/* GPIO base & offsets for I2C bit-bang */
#define GPIO1_BASE_ADDR 0x4804C000
#define GPIO_MEM_SIZE   0x1000
#define GPIO_OE_OFFSET        0x134
#define GPIO_SET_OFFSET       0x194
#define GPIO_CLR_OFFSET       0x190
#define GPIO_IN_OFFSET        0x138

#define SCL_PIN   17
#define SDA_PIN   16
#define SCL_MASK  (1 << SCL_PIN)
#define SDA_MASK  (1 << SDA_PIN)

/* Timing */
#define I2C_DELAY_US 5
#define DEFAULT_REFRESH_INTERVAL 1000 /* ms */

/* Globals */
static int major;
static struct class *bh_class = NULL;
static struct device *bh_device = NULL;
static struct cdev bh_cdev;
static void __iomem *gpio_base;

/* Sensor data */
struct bh_sensor {
    unsigned int lux;
    unsigned long last_update;
    struct mutex lock;
    bool initialized;
    bool cont_mode;
};
static struct bh_sensor sensor;

/* Module params */
static unsigned int refresh_interval = DEFAULT_REFRESH_INTERVAL;
static bool auto_refresh = true;
module_param(refresh_interval, uint, 0644);
MODULE_PARM_DESC(refresh_interval, "Refresh interval in ms");
module_param(auto_refresh, bool, 0644);
MODULE_PARM_DESC(auto_refresh, "Enable auto refresh");

static unsigned char meas_mode = CMD_CONT_HIGH;

/* I2C bit-bang helpers */
static inline void gpio_set_clock(int val)
{
    if (val)
        iowrite32(SCL_MASK, gpio_base + GPIO_SET_OFFSET);
    else
        iowrite32(SCL_MASK, gpio_base + GPIO_CLR_OFFSET);
    udelay(I2C_DELAY_US);
}

static inline void gpio_set_data(int val)
{
    if (val)
        iowrite32(SDA_MASK, gpio_base + GPIO_SET_OFFSET);
    else
        iowrite32(SDA_MASK, gpio_base + GPIO_CLR_OFFSET);
    udelay(I2C_DELAY_US);
}

static inline void configure_sda_in(void)
{
    u32 reg = ioread32(gpio_base + GPIO_OE_OFFSET);
    iowrite32(reg | SDA_MASK, gpio_base + GPIO_OE_OFFSET);
    udelay(I2C_DELAY_US);
}

static inline void configure_sda_out(void)
{
    u32 reg = ioread32(gpio_base + GPIO_OE_OFFSET);
    iowrite32(reg & ~SDA_MASK, gpio_base + GPIO_OE_OFFSET);
    udelay(I2C_DELAY_US);
}

static inline int gpio_read_data(void)
{
    u32 reg = ioread32(gpio_base + GPIO_IN_OFFSET);
    return (reg & SDA_MASK) ? 1 : 0;
}

static void i2c_begin(void)
{
    configure_sda_out();
    gpio_set_data(1);
    gpio_set_clock(1);
    gpio_set_data(0);
    gpio_set_clock(0);
}

static void i2c_end(void)
{
    configure_sda_out();
    gpio_set_data(0);
    gpio_set_clock(1);
    gpio_set_data(1);
}

static int i2c_send_byte(unsigned char byte)
{
    int i, ack;
    configure_sda_out();
    for (i = 7; i >= 0; i--) {
        gpio_set_data((byte >> i) & 1);
        gpio_set_clock(1);
        gpio_set_clock(0);
    }
    configure_sda_in();
    gpio_set_clock(1);
    ack = !gpio_read_data();
    gpio_set_clock(0);
    return ack;
}

static unsigned char i2c_receive_byte(int ack)
{
    int i;
    unsigned char byte = 0;
    configure_sda_in();
    for (i = 7; i >= 0; i--) {
        gpio_set_clock(1);
        if (gpio_read_data())
            byte |= (1 << i);
        gpio_set_clock(0);
    }
    configure_sda_out();
    gpio_set_data(!ack);
    gpio_set_clock(1);
    gpio_set_clock(0);
    return byte;
}

/* BH1750 communication */
static int bh1750_send_command(unsigned char cmd)
{
    int ret;
    i2c_begin();
    ret = i2c_send_byte(BH1750_ADDR << 1);
    if (!ret) { i2c_end(); return -EIO; }
    ret = i2c_send_byte(cmd);
    if (!ret) { i2c_end(); return -EIO; }
    i2c_end();
    return 0;
}

static int bh1750_get_raw_value(unsigned short *value)
{
    unsigned char msb, lsb;
    int ret;
    i2c_begin();
    ret = i2c_send_byte((BH1750_ADDR << 1) | 1);
    if (!ret) { i2c_end(); return -EIO; }
    msb = i2c_receive_byte(1);
    lsb = i2c_receive_byte(0);
    i2c_end();
    *value = (msb << 8) | lsb;
    return 0;
}

static unsigned int bh1750_get_wait_time(unsigned char mode)
{
    switch (mode) {
    case CMD_CONT_HIGH:
    case CMD_CONT_HIGH2:
    case CMD_ONETIME_HIGH:
        return 180;
    case CMD_CONT_LOW:
    case CMD_ONETIME_LOW:
        return 24;
    default:
        return 180;
    }
}

static bool bh1750_is_continuous_mode(unsigned char mode)
{
    return (mode == CMD_CONT_HIGH || mode == CMD_CONT_HIGH2 || mode == CMD_CONT_LOW);
}

static int bh1750_sensor_init(void)
{
    int ret;
    ret = bh1750_send_command(CMD_POWER_ON);
    if (ret < 0)
        return ret;
    ret = bh1750_send_command(CMD_RESET);
    if (ret < 0)
        return ret;
    ret = bh1750_send_command(meas_mode);
    if (ret < 0)
        return ret;
    msleep(bh1750_get_wait_time(meas_mode));
    sensor.initialized = true;
    sensor.cont_mode = bh1750_is_continuous_mode(meas_mode);
    return 0;
}

static int bh1750_read_lux_value(unsigned int *lux)
{
    unsigned short raw;
    int ret;

    mutex_lock(&sensor.lock);
    if (!sensor.initialized) {
        ret = bh1750_sensor_init();
        if (ret < 0) {
            mutex_unlock(&sensor.lock);
            return ret;
        }
    } else if (!sensor.cont_mode) {
        ret = bh1750_send_command(meas_mode);
        if (ret < 0) {
            mutex_unlock(&sensor.lock);
            return ret;
        }
        msleep(bh1750_get_wait_time(meas_mode));
    }
    ret = bh1750_get_raw_value(&raw);
    if (ret < 0) {
        mutex_unlock(&sensor.lock);
        return ret;
    }
    *lux = (raw * 10) / 12;
    sensor.lux = *lux;
    sensor.last_update = jiffies;
    mutex_unlock(&sensor.lock);
    return 0;
}

/* Timer refresh */
static struct timer_list refresh_timer;
static void sensor_refresh_callback(struct timer_list *t)
{
    unsigned int lux;
    if (auto_refresh)
        bh1750_read_lux_value(&lux);
    mod_timer(&refresh_timer, jiffies + msecs_to_jiffies(refresh_interval));
}

/* File operations */
static int bh1750_dev_open(struct inode *inode, struct file *file)
{
    pr_info("BH1750: Device opened\n");
    return 0;
}

static int bh1750_dev_close(struct inode *inode, struct file *file)
{
    pr_info("BH1750: Device closed\n");
    return 0;
}

static ssize_t bh1750_dev_read(struct file *file, char __user *buf, size_t count, loff_t *offset)
{
    unsigned int lux;
    int ret;
    char lux_str[32];
    int len;

    if (*offset > 0)
        return 0;

    if (time_after(jiffies, sensor.last_update + msecs_to_jiffies(refresh_interval)) || !auto_refresh) {
        ret = bh1750_read_lux_value(&lux);
        if (ret < 0)
            return ret;
    } else {
        lux = sensor.lux;
    }

    len = snprintf(lux_str, sizeof(lux_str), "%u\n", lux);
    if (len > count)
        return -EINVAL;
    if (copy_to_user(buf, lux_str, len))
        return -EFAULT;

    *offset += len;
    return len;
}

static ssize_t bh1750_dev_write(struct file *file, const char __user *buf, size_t count, loff_t *offset)
{
    char kbuf[32];
    unsigned int val;
    int ret;

    if (count >= sizeof(kbuf))
        return -EINVAL;
    if (copy_from_user(kbuf, buf, count))
        return -EFAULT;

    kbuf[count] = '\0';
    ret = kstrtouint(kbuf, 10, &val);
    if (ret)
        return ret;

    if (val < 10 || val > 60000)
        return -EINVAL;

    refresh_interval = val;
    pr_info("BH1750: Refresh interval set to %u ms\n", refresh_interval);
    *offset = count;
    return count;
}

static const struct file_operations bh1750_fops = {
    .owner = THIS_MODULE,
    .open = bh1750_dev_open,
    .release = bh1750_dev_close,
    .read = bh1750_dev_read,
    .write = bh1750_dev_write,
};

/* Module init/exit */
static int __init bh1750_init(void)
{
    int ret;
    dev_t devt;

    /* Map GPIO */
    gpio_base = ioremap(GPIO1_BASE_ADDR, GPIO_MEM_SIZE);
    if (!gpio_base) {
        pr_err("BH1750: Failed to ioremap GPIO base\n");
        return -ENOMEM;
    }

    /* Set GPIO pins to output and high */
    {
        u32 reg = ioread32(gpio_base + GPIO_OE_OFFSET);
        reg &= ~(SCL_MASK | SDA_MASK); // output
        iowrite32(reg, gpio_base + GPIO_OE_OFFSET);
        iowrite32(SCL_MASK | SDA_MASK, gpio_base + GPIO_SET_OFFSET);
    }

    /* Register char device */
    ret = alloc_chrdev_region(&devt, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("BH1750: Failed to alloc chrdev\n");
        iounmap(gpio_base);
        return ret;
    }
    major = MAJOR(devt);

    cdev_init(&bh_cdev, &bh1750_fops);
    bh_cdev.owner = THIS_MODULE;
    ret = cdev_add(&bh_cdev, devt, 1);
    if (ret < 0) {
        unregister_chrdev_region(devt, 1);
        iounmap(gpio_base);
        return ret;
    }

    bh_class = class_create(CLASS_NAME);
    if (IS_ERR(bh_class)) {
        cdev_del(&bh_cdev);
        unregister_chrdev_region(devt, 1);
        iounmap(gpio_base);
        return PTR_ERR(bh_class);
    }

    bh_device = device_create(bh_class, NULL, devt, NULL, DEVICE_NAME);
    if (IS_ERR(bh_device)) {
        class_destroy(bh_class);
        cdev_del(&bh_cdev);
        unregister_chrdev_region(devt, 1);
        iounmap(gpio_base);
        return PTR_ERR(bh_device);
    }

    mutex_init(&sensor.lock);
    sensor.initialized = false;
    sensor.lux = 0;

    timer_setup(&refresh_timer, sensor_refresh_callback, 0);
    mod_timer(&refresh_timer, jiffies + msecs_to_jiffies(refresh_interval));

    pr_info("BH1750: Module loaded, major=%d\n", major);
    return 0;
}

static void __exit bh1750_exit(void)
{
    dev_t devt = MKDEV(major, 0);
    del_timer_sync(&refresh_timer);

    if (bh_device)
        device_destroy(bh_class, devt);
    if (bh_class)
        class_destroy(bh_class);

    cdev_del(&bh_cdev);
    unregister_chrdev_region(devt, 1);

    if (gpio_base)
        iounmap(gpio_base);

    pr_info("BH1750: Module unloaded\n");
}
module_init(bh1750_init);
module_exit(bh1750_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Toan");
MODULE_DESCRIPTION("BH1750 Driver");
