#############################################################
#
# winiot system
#
#############################################################


WINIOT_VERSION = 1.0
WINIOT_SITE = $(TOPDIR)/../../noobs-preinstallation-tool/ffutools
WINIOT_SITE_METHOD = local
WINIOT_LICENSE = BSD-3c
WINIOT_LICENSE_FILES = LICENSE.txt
WINIOT_INSTALL_STAGING = NO

define WINIOT_BUILD_CMDS
	$(MAKE) CC="$(TARGET_CC)" LD="$(TARGET_LD)" -C $(@D) all
endef

define WINIOT_INSTALL_TARGET_CMDS
endef

$(eval $(generic-package))
