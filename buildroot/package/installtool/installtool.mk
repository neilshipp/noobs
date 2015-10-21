#############################################################
#
# installtool system
#
#############################################################


INSTALLTOOL_VERSION = 1.0
INSTALLTOOL_SITE = $(TOPDIR)/../../noobs-preinstallation-tool/checktool
INSTALLTOOL_SITE_METHOD = local
INSTALLTOOL_LICENSE = BSD-3c
INSTALLTOOL_LICENSE_FILES = LICENSE.txt
INSTALLTOOL_INSTALL_STAGING = NO
INSTALLTOOL_DEPENDENCIES = qt qjson

define INSTALLTOOL_BUILD_CMDS
	(cd $(@D) ; $(QT_QMAKE))
	$(MAKE) -C $(@D) all
#	$(TARGET_STRIP) $(@D)/installtool
endef

define INSTALLTOOL_INSTALL_TARGET_CMDS
endef

$(eval $(generic-package))
