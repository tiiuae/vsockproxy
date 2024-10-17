# About vsockproxy

VM Sockets (vsock) is a communication mechanism between guest virtual machines and the host.

This tool makes it possible to use vsock for guest to guest communication.

It listens for incoming connections on host, connects to the guest virtual machine and forwards data in both directions.

# Building

Initialize the build:
```
meson setup build
cd build
```

Build the executable:
```
meson compile
```

# Usage

```
vsockproxy <local_port> <remote_cid> <remote_port> <allowed_cid>
```

local_port - port number on host where to listen for incoming connections from guest virtual machines

remote_cid - CID of the vsock in the guest virtual machine where the data will be forwarded

remote_port - port number of the vsock in the guest virtual machine where the data will be forwarded

allowed_cid - CID of the guest virtual machine that is allowed to connect (0 means any); all other connections will be denied.
