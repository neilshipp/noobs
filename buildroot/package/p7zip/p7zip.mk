############################################
#
# p7zip
# 
# Note: I just constructed this (mostly) from 
# Gentoo ebuild file of p7zip.
#
# Original can be found from here:
# http://sources.gentoo.org/cgi-bin/viewvc.cgi/gentoo-x86/app-arch/p7zip/p7zip-9.20.1-r2.ebuild
# 
############################################
P7ZIP_VERSION = 9.20.1
P7ZIP_SOURCE = p7zip_$(P7ZIP_VERSION)_src_all.tar.bz2
P7ZIP_SITE = http://downloads.sourceforge.net/project/p7zip/p7zip/$(P7ZIP_VERSION)
P7ZIP_DEPENDENCIES = host-yasm
# What UnRAR license basically means here is that as long
# as you are not using UnRAR source code to reverse engineer 
# the RAR *compression* algorithm, which is proprietary and owned by Alexander L. Roshal,
# then everything should be fine, in this case (LGPLv2.1+).
P7ZIP_LICENSE = LGPLv2.1+ UnRAR

define	P7ZIP_POST_CONFIGURE_FIXUP
	(cd $(@D); \
	 cp makefile.linux_any_cpu_gcc_4.X makefile.machine)
endef

P7ZIP_POST_CONFIGURE_HOOKS += P7ZIP_POST_CONFIGURE_FIXUP

define	P7ZIP_CONFIGURE_CMDS
	# Do some PCH disabling and makefile fixup
	(cd $(@D); \
	sed "s:PRE_COMPILED_HEADER=StdAfx.h.gch:PRE_COMPILED_HEADER=:g" -i makefile.* ;\
	sed -e 's:-m32 ::g' -e 's:-m64 ::g' -e 's:-O::g' -e 's:-pipe::g' \
	-e "/^CC/s:\$$(ALLFLAGS):$${CFLAGS} \$$(ALLFLAGS):g" \
	-e "/^CXX/s:\$$(ALLFLAGS):$${CXXFLAGS} \$$(ALLFLAGS):g" -i makefile* ;\
	sed -i -e "/^CXX=/s:g++:$(TARGET_CXX):" \
		-e "/^CC=/s:gcc:$(TARGET_CC):" \
		-e '/ALLFLAGS/s:-s ::' makefile* )
endef

define P7ZIP_BUILD_CMDS
	 $(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) $(MAKE) -C $(@D) all2
endef

define P7ZIP_INSTALL_TARGET_CMDS
	mkdir -p $(TARGET_DIR)/usr/lib/p7zip
	cp -r $(@D)/bin/* $(TARGET_DIR)/usr/lib/p7zip/

	# According to Gentoo folks, we can't just use symlinks
	# but have to invoke p7zip programs (7z & 7za) with full path (/usr/lib/p7zip) 
	# when using them (which is really annoying)

	# That's why we use more helpful wrapper scripts instead.
	cp package/p7zip/7z $(TARGET_DIR)/usr/bin/
	cp package/p7zip/7za $(TARGET_DIR)/usr/bin/
endef

$(eval $(generic-package))
# $(eval $(host-generic-package))


