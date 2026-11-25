"""SSH over Bluetooth LE: ssh ProxyCommand client for ble_ssh_server."""

__version__ = "0.1.3"

# Must match src/ble_ssh.h in the C implementation.
SERVICE_UUID = "5f2d0001-8c3b-4a7e-b1d9-2e6f4a9c7b10"
RX_UUID = "5f2d0002-8c3b-4a7e-b1d9-2e6f4a9c7b10"  # client -> sshd, write
TX_UUID = "5f2d0003-8c3b-4a7e-b1d9-2e6f4a9c7b10"  # sshd -> client, notify
