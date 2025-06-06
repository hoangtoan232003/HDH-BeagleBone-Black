BEAGLEBONE_AUTO_VERSION = 1.0
BEAGLEBONE_AUTO_SITE = $(BEAGLEBONE_AUTO_PKGDIR)
BEAGLEBONE_AUTO_SITE_METHOD = local
BEAGLEBONE_AUTO_INSTALL_TARGET = YES

# Thêm flag và thư viện khi biên dịch app.c
APP_CFLAGS = -I$(@D)/include
APP_LDFLAGS = -L$(STAGING_DIR)/usr/lib -lmosquitto -lcjson -lpthread

define BEAGLEBONE_AUTO_BUILD_CMDS
	# Biên dịch app.c
	$(TARGET_CC) $(APP_CFLAGS) -o $(@D)/app $(@D)/app.c $(APP_LDFLAGS)

	# Biên dịch driver kernel
	$(MAKE) -C $(LINUX_DIR) M=$(@D) ARCH=$(KERNEL_ARCH) CROSS_COMPILE=$(TARGET_CROSS) modules
endef

define BEAGLEBONE_AUTO_INSTALL_TARGET_CMDS
	# Cài app vào rootfs
	$(INSTALL) -D -m 0755 $(@D)/app $(TARGET_DIR)/usr/bin/app

	# Cài các driver kernel
	$(INSTALL) -D -m 0755 $(@D)/bh1750_1.ko $(TARGET_DIR)/lib/modules/$(LINUX_VERSION)/bh1750_1.ko
	$(INSTALL) -D -m 0755 $(@D)/led.ko     $(TARGET_DIR)/lib/modules/$(LINUX_VERSION)/led.ko
	$(INSTALL) -D -m 0755 $(@D)/dht11.ko   $(TARGET_DIR)/lib/modules/$(LINUX_VERSION)/dht11.ko

	# Cài script khởi động
	$(INSTALL) -D -m 0755 $(@D)/S99beaglebone $(TARGET_DIR)/etc/init.d/S99beaglebone
endef
