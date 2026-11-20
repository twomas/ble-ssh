################################################################################
#
# ble-ssh
#
################################################################################

BLE_SSH_VERSION = v0.1.2
BLE_SSH_SITE = https://github.com/twomas/ble-ssh.git
BLE_SSH_SITE_METHOD = git
BLE_SSH_GIT_SUBMODULES = YES
BLE_SSH_LICENSE = MIT
BLE_SSH_LICENSE_FILES = LICENSE external/bluez_inc/LICENSE
BLE_SSH_DEPENDENCIES = host-pkgconf libglib2 bluez5_utils

BLE_SSH_CONF_OPTS = \
	-DPKG_CONFIG_EXECUTABLE="$(PKG_CONFIG_HOST_BINARY)" \
	-DBLE_SSH_SYSCONFDIR=/etc

# Created at build time; the sysusers.d file that is also installed is then a no-op.
define BLE_SSH_USERS
	ble-ssh -1 ble-ssh -1 * - - - SSH over Bluetooth LE
endef

ifeq ($(BR2_PACKAGE_BLE_SSH_CLIENT),)
define BLE_SSH_REMOVE_CLIENT
	rm -f $(TARGET_DIR)/usr/bin/ble_ssh_client
endef
BLE_SSH_POST_INSTALL_TARGET_HOOKS += BLE_SSH_REMOVE_CLIENT
endif

define BLE_SSH_INSTALL_DEFAULTS
	mkdir -p $(TARGET_DIR)/etc/default
	echo 'BLE_SSH_OPTS="$(call qstrip,$(BR2_PACKAGE_BLE_SSH_OPTS))"' > $(TARGET_DIR)/etc/default/ble-ssh
endef
BLE_SSH_POST_INSTALL_TARGET_HOOKS += BLE_SSH_INSTALL_DEFAULTS

# The unit itself is installed by `cmake --install` (prefix /usr); enable it here,
# matching its [Install] section.
define BLE_SSH_INSTALL_INIT_SYSTEMD
	mkdir -p $(TARGET_DIR)/etc/systemd/system/multi-user.target.wants
	mkdir -p $(TARGET_DIR)/etc/systemd/system/bluetooth.service.wants
	ln -sf /usr/lib/systemd/system/ble-ssh-server.service \
		$(TARGET_DIR)/etc/systemd/system/multi-user.target.wants/ble-ssh-server.service
	ln -sf /usr/lib/systemd/system/ble-ssh-server.service \
		$(TARGET_DIR)/etc/systemd/system/bluetooth.service.wants/ble-ssh-server.service
endef

$(eval $(cmake-package))
