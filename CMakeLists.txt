cmake_minimum_required(VERSION 3.16)
project(ble_ssh C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_EXTENSIONS ON)

include(GNUInstallDirs)

# Where the generated unit is installed (relative to the prefix, or absolute). Not LIBDIR:
# on Debian that is multiarch (lib/aarch64-linux-gnu), but systemd only looks in lib/.
set(BLE_SSH_SYSTEMD_UNITDIR "lib/systemd/system" CACHE STRING "systemd system unit directory")
set(BLE_SSH_SYSUSERSDIR "lib/sysusers.d" CACHE STRING "systemd sysusers.d directory")
# The unit reads optional settings from <sysconfdir>/default/ble-ssh; /etc on every normal system.
set(BLE_SSH_SYSCONFDIR "/etc" CACHE PATH "System configuration directory used by the systemd unit")

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED glib-2.0 gio-2.0 gio-unix-2.0)
include_directories(${GLIB_INCLUDE_DIRS})

# Only the bluez_inc library, not its examples/tests. EXCLUDE_FROM_ALL keeps its headers and
# static library out of `cmake --install`; it is still built because our targets link to it.
add_subdirectory(external/bluez_inc/binc EXCLUDE_FROM_ALL)

add_executable(ble_ssh_server src/ble_ssh_server.c src/sd_notify.c)
target_link_libraries(ble_ssh_server Binc)

add_executable(ble_ssh_client src/ble_ssh_client.c)
target_link_libraries(ble_ssh_client Binc)

# Strict warnings for our own code only; bluez_inc keeps its upstream flags.
foreach(target ble_ssh_server ble_ssh_client)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
endforeach()

# ExecStart must point at the installed binary, so generate the unit from the install paths.
configure_file(systemd/ble-ssh-server.service.in ${CMAKE_CURRENT_BINARY_DIR}/ble-ssh-server.service @ONLY)

install(TARGETS ble_ssh_server ble_ssh_client RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ble-ssh-server.service DESTINATION ${BLE_SSH_SYSTEMD_UNITDIR})
install(FILES systemd/ble-ssh.sysusers DESTINATION ${BLE_SSH_SYSUSERSDIR} RENAME ble-ssh.conf)
