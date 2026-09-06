# Aeron network probe

Checks room discovery, ICE connectivity and DirectPlay ping/pong delivery between
two computers, without game assets.

Build Linux or Windows packages with Docker using the `netprobe-artifact` target
in OpenXvT's `packaging/README.md`. Extract the whole archive on each computer.
Linux requires Debian 12 or compatible with `libcurl4`, `ca-certificates` and
the GCC C++ runtime installed.

Copy `netprobe.example.yaml` to `netprobe.yaml` on both computers. Set `lobby_url`
to your HTTPS lobby service and use the same unique `room_name`.

On computer A:

```sh
./aeron-netprobe host netprobe.yaml
```

Wait for `Hosting ...`, then on computer B:

```sh
./aeron-netprobe list netprobe.yaml
./aeron-netprobe join netprobe.yaml
```

On Windows, use `.\aeron-netprobe.exe`. Run the executables directly on the
computers so ICE sees their network interfaces.

The client sends ten pings and reports round-trip times, reply counts and whether
the selected path is direct or relayed through TURN. Exit status is 0 if at least
one reply succeeds, 1 on failure and 2 for invalid usage. Stop the host with Ctrl+C.
