# Virtual Network Switch

A multithreaded software switch for Linux. Each host occupies a numbered TCP port slot. The switch learns source MAC addresses and forwards frames within VLANs. A local admin tool manages live ports and reads per-port statistics.

## Build

On Linux with GCC and GNU Make, run:

```sh
make
./test
```

`make` builds `switch`, `host`, `swctl`, and `test` with C11, POSIX threads, and warnings treated as errors. `./test` runs the bundled frame and MAC-table checks. `make clean` removes the executables and object files.

## Run a local switch

Start the switch with a TCP listening port and 1 to 16 host slots. Slot IDs start at 0.

```sh
./switch 9000 4
```

From two other terminals:

```sh
./host 127.0.0.1 9000 02:00:00:00:00:01
./host 127.0.0.1 9000 02:00:00:00:00:02
```

Run each host in its own terminal. Once both are connected, enter `02:00:00:00:00:02 hello` in the first host. An unknown destination is flooded within its VLAN. When the second host replies using `02:00:00:00:00:01 reply`, the switch learns its address and can forward later traffic directly. Use destination `FF:FF:FF:FF:FF:FF` for broadcast. The host CLI sends normal-priority frames. An extra connection waits up to two seconds for an available slot before receiving `switch full`.

When a port exceeds eight distinct learned source MAC addresses, the switch drops the offending frame, disconnects the client, and keeps the port down. Run `./swctl up <port>` to make the slot available for a new connection.

## Admin commands

Run `swctl` from the switch's working directory, or set the same `SWITCH_RUNTIME_DIR` for both programs.

| Command | Effect |
| --- | --- |
| `./swctl show ports` | List each port's state, VLAN or trunk role, mirror target, and rate. |
| `./swctl show mac` | List learned MAC addresses with their VLANs and ports. |
| `./swctl shutdown <port>` | Disable a port and give its queue up to two seconds to drain before forcing disconnect. |
| `./swctl up <port>` | Re-enable a disabled port for new connections. |
| `./swctl vlan <port> <vlan>` | Assign an access VLAN to a port. |
| `./swctl trunk <port>` | Set the trunk port. A previous trunk returns to access VLAN 1. |
| `./swctl mirror <port> <sniffer_port>` | Send ingress and egress copies to the sniffer, including across VLANs. |
| `./swctl mirror <port> off` | Disable mirroring for the selected port. |
| `./swctl clear mac` | Clear learned MAC entries. |
| `./swctl stats` | Show incoming, outgoing, dropped, and mirrored frame counts by port. |
| `./swctl stats --watch` | Read the shared counters about once per second. Press Ctrl+C to stop the watch. |

## Configuration

The optional `switch.conf` is read at startup. Put one setting on each line. Blank lines and text after `#` are ignored. If the default file is absent at startup, all ports start in VLAN 1 with a rate of 100 frames per second, and mirroring is off.

This example uses the four slots from `./switch 9000 4`:

```text
vlan 0 10
vlan 1 10
trunk 2
vlan 3 20
mirror 0 3
rate 1 50
```

VLAN IDs range from 1 to 4094. Access ports assign incoming frames to their configured VLAN. The single optional trunk preserves VLAN tags. The mirror target must be a different slot and can be set to `off`. A rate is a whole number from 1 to 1,000,000 frames per second.

`SWITCH_RUNTIME_DIR` defaults to the current directory and can point to an existing directory for `switch.sock`, `switch.log`, `mac_table.txt`, and the default configuration file. Set `SWITCH_CONFIG` to select a different configuration file. Use the same runtime directory for the switch and its admin clients.

## Signals

- Ctrl+C or `kill -INT <switch_pid>` shuts down the switch cleanly.
- `kill -HUP <switch_pid>` reloads the configuration. A successful reload reapplies file settings and defaults, and preserves port up/down state. A failed reload keeps the current configuration.
- `kill -USR1 <switch_pid>` writes a snapshot to `mac_table.txt`.

See the [project report](output/pdf/virtual-switch-report.pdf) for the thread diagram, lock order, and implementation notes.
