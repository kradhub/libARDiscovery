LOCAL_PATH := $(call my-dir)

# Not with bionic
ifneq ("$(TARGET_LIBC)","bionic")

include $(CLEAR_VARS)

LOCAL_CATEGORY_PATH := dragon/libs
LOCAL_MODULE := libARDiscovery
LOCAL_DESCRIPTION := ARSDK Discovery and Connection Management Layer

LOCAL_LIBRARIES := libARSAL avahi
LOCAL_EXPORT_LDLIBS := -lardiscovery

#Autotools variables
LOCAL_AUTOTOOLS_CONFIGURE_ARGS := --with-libARSALInstallDir=""

ifeq ("$(TARGET_PBUILD_FORCE_STATIC)","1")
LOCAL_AUTOTOOLS_CONFIGURE_ARGS += --disable-shared
endif


# User define command to be launch before configure step.
# Generates files used by configure
define LOCAL_AUTOTOOLS_CMD_POST_UNPACK
	$(Q) cd $(PRIVATE_SRC_DIR) && ./bootstrap
endef

# User define command to be launch after dirclean
# Remove every files generated by bootstrap
define LOCAL_AUTOTOOLS_CMD_POST_DIRCLEAN
	$(Q) cd $(PRIVATE_SRC_DIR) && ./cleanup
endef

include $(BUILD_AUTOTOOLS)

endif
