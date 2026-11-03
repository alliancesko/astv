# astv

[English](README.md) | [Русский](README.ru.md)

Current version: `0.2.0`.

`astv` is a terminal monitor for the Asterisk Manager Interface (AMI). Its
screen is divided into four panels:

- top left, 3/5 width and 3/4 height: all active Asterisk channels
- top right, 2/5 width and 3/4 height: `chan_sip`, `chan_pjsip`, and
  `chan_iax2` peers/endpoints
- bottom left, 3/5 width and 1/4 height: all AMI events
- bottom right, 2/5 width and 1/4 height: AMI events for the selected channel

The channel list groups calls by `Linkedid`. Registration changes are shown
for `chan_sip`, `chan_pjsip`, and `chan_iax2`. On startup, `astv` requests
SIP and IAX2 peers, PJSIP endpoints, PJSIP inbound contacts, and outbound
registration snapshots.
Channel creation, state, dial, bridge, and hangup events automatically refresh
the peer and endpoint lists after a one-second debounce. `DeviceStateChange`
events update matching SIP, PJSIP, and IAX2 entries immediately. For outgoing
calls, both `Channel` and `DestChannel` are used to mark the originating
device and destination trunk as `In use` before the next full refresh.
Peers and endpoints are sorted case-insensitively by name, so status changes
do not move rows around.

The initial version is read-only. It does not originate, redirect, or hang up
calls.

## Asterisk compatibility

On every AMI login, `astv` requests `CoreSettings` and reads
`AsteriskVersion` and `AMIversion`. The detected Asterisk version is shown in
the `CHANNELS` panel title. Version-aware capability selection is combined
with runtime action responses, because channel modules may be disabled on any
Asterisk version.

The supported LTS profiles are:

```text
Asterisk 16       CoreShowChannels, explicit PJSIP contacts
Asterisk 18       CoreShowChannels; Status bridge supplement from 18.26
Asterisk 20       CoreShowChannels; Status bridge supplement from 20.11
Asterisk 22       CoreShowChannels; Status bridge supplement from 22.1
```

`Status.BridgeID` supplements `CoreShowChannel.BridgeId` where that field is
available, while live `BridgeEnter` and `BridgeLeave` events keep bridge
membership current.
PJSIP endpoint and contact totals are finalized from `EndpointListComplete`
and `ContactListComplete`, including servers that omit `ActionID` on list
events. If `PJSIPShowContacts` is unavailable but `EndpointList` includes a
`Contacts` field, `astv` counts the embedded SIP/SIPS URIs as a fallback. If
version detection fails, `astv` uses capability probes after a short timeout.

## Build

```sh
make
make test
```

The project requires a C11 compiler and standard Linux/POSIX headers. It has no
third-party runtime dependencies. The build links POSIX threads with `-pthread`;
ncurses is not required. Use a UTF-8 locale and a terminal with Unicode
box-drawing support.

Install build tools on Debian/Ubuntu:

```sh
sudo apt install build-essential
```

On AlmaLinux/Rocky Linux/Fedora:

```sh
sudo dnf install gcc make glibc-devel
```

Build from a local checkout with `make`. Optionally install the binary:

```sh
sudo install -m 0755 astv /usr/local/bin/astv
sudo astv
```

There is no `make install` target. To remove an installed binary, remove
`/usr/local/bin/astv`. Credentials remain in `/etc/astv/astv.conf` until removed
separately. Build on the target distribution when using older glibc versions;
one prebuilt binary is not guaranteed to run on every Linux release.

## Asterisk configuration

Add a restricted user to `/etc/asterisk/manager.conf`:

```ini
[general]
enabled = yes
port = 5038
bindaddr = 127.0.0.1

[astv]
secret = replace-with-a-long-random-secret
read = system,call,reporting,command
write = system,call,reporting
permit = 127.0.0.1/255.255.255.255
```

Reload AMI:

```sh
asterisk -rx "manager reload"
asterisk -rx "manager show users"
```

Adjust `permit` only when a remote client must connect. Do not expose plain AMI
port 5038 to the public Internet. Use a VPN or an SSH tunnel. The current client
uses plain TCP and does not implement native AMI TLS.

The example AMI account is named `astv`, while interactive setup defaults to
`monitor`: enter `astv` during setup when using this example. AMI `write`
permissions authorize snapshot actions even though astv does not modify calls.
Available actions also depend on loaded Asterisk modules and account permissions.

## First run

By default, `astv` reads `/etc/astv/astv.conf`. If the file does not exist,
the first run starts an interactive setup and asks for:

- AMI address, default `127.0.0.1`
- AMI port, default `5038`
- AMI username, default `monitor`
- AMI password

The program requires administrator privileges:

```sh
sudo ./astv
```

The generated configuration remains owned by root and is created with mode
`0600`.

The generated file has this format:

```ini
host=127.0.0.1
port=5038
username=astv
secret=replace-with-a-long-random-secret
```

## Run

After initial configuration:

```sh
sudo ./astv
```

`astv` requires effective root privileges. If started by an unprivileged
user, it exits with status `1` and prints a command suggesting `sudo`.

Before opening the terminal interface, `astv` connects to the configured
server and waits up to five seconds for successful AMI authentication. A
network error, timeout, or authentication failure is printed to standard
error and the program exits with status `1` without drawing the monitor
screen.

Values may be overridden without changing the configuration:

Using an environment variable:

```sh
sudo env ASTV_SECRET='replace-with-a-long-random-secret' ./astv \
  --host 127.0.0.1 --user astv
```

Using a protected file:

```sh
printf '%s\n' 'replace-with-a-long-random-secret' > ~/.astv-secret
chmod 600 ~/.astv-secret
sudo ./astv --host 127.0.0.1 --user astv --secret-file ~/.astv-secret
```

Options:

```text
-H, --host HOST
-p, --port PORT
-u, --user USER
-s, --secret-file FILE
-c, --config FILE
-h, --help
```

Controls:

```text
Tab         move focus to the next panel
Shift+Tab   move focus to the previous panel
Up / Down   move the cursor or scroll the focused panel
PgUp/PgDown move one page in the focused panel
Left/Right  scroll the selected row horizontally
Home / End  move to the first or last item
h / Esc     open or close the help window
q           quit
```

Top panel headers:

```text
CH:N         active Asterisk channels
CL:N         calls grouped by Linkedid
AST:V        detected Asterisk version
[+]          AMI connected
[!]          AMI interrupted; waiting for the next retry
[?]          reconnect or AMI Login is in progress
PEERS: N     total SIP, IAX2, and PJSIP entries
SIP:s/N      SIP request status and peer count
IAX2:s/N     IAX2 request status and peer count
PJSIP:+ E:N C:N/-/?
             endpoints work; contacts show count, error, or pending
PJSIP:-/?
             endpoint request error or pending
```

The status symbol is `+` for success, `-` for an AMI request error, and `?`
while the request is pending.

After an established AMI connection is lost, `astv` keeps the last panel
state and performs up to three reconnect attempts, three seconds apart. If
all three attempts fail, a modal error is displayed and only `q` exits.

Each overflowing panel displays a scrollbar. Event panels follow new events
while positioned at the last entry; scrolling upward pauses automatic
following until the cursor returns to the end. Up to 4096 AMI events are kept
in memory. The selected row displays `<` or `>` at its edges when hidden text
exists to the left or right.

The centered help window contains controls, command-line options, author
information, and a working `manager.conf` example. AMI monitoring and event
collection continue while the help window is open. The `q` key is ignored
inside help to prevent accidental program termination.

The same information is available without root privileges, configuration, or
an AMI connection:

```sh
./astv --help
```

The terminal must be at least 72 columns by 16 rows.

## Remote connection over SSH

Create a local tunnel:

```sh
ssh -L 5038:127.0.0.1:5038 user@asterisk-server
```

Then run `astv` against `127.0.0.1:5038`.

Use a different local port if 5038 is already occupied, for example
`ssh -N -L 15038:127.0.0.1:5038 user@asterisk-server`, then pass
`--host 127.0.0.1 --port 15038` to astv.

## Configuration precedence

The configuration file is loaded first; a missing file triggers interactive
setup even when overrides are supplied. `--host`, `--port`, and `--user`
override the corresponding file values. `ASTV_SECRET` overrides the configured
password, and `--secret-file` takes precedence over both. Overrides are not
saved. With sudo, pass the environment explicitly as shown above.

## Troubleshooting and limits

- Connection refused or timed out: check the address, AMI listener, firewall,
  tunnel, and `enabled` in `manager.conf`.
- Authentication failed: check the account name, secret, and `permit` rule.
- A protocol shows `-`: its AMI action was rejected or timed out; check loaded
  channel modules and AMI permissions. A disabled chan_sip is normal on a
  PJSIP-only server.
- Broken borders: check terminal font and UTF-8 locale. Minimum size is 72x16.
- No configuration in noninteractive use: create it through interactive setup
  first, or provide an existing file with `--config`.

Channel and peer tables each have a fixed capacity of 1024 entries. History is
a 4096-entry in-memory ring: oldest events are overwritten, and history is not
persisted across restarts. Snapshot traffic and RTCP events are filtered from
the event view. Reconnection restores current state, but cannot recover events
missed while disconnected. Version profiles describe implemented compatibility,
not a guarantee that every distribution build has been tested.

## Source layout

- `src/main.c`: AMI session, peer snapshots, TUI, and keyboard input.
- `src/ami.c`: streaming AMI parser.
- `src/asterisk_compat.c`: version detection and compatibility decisions.
- `src/channel_store.c`: channel state and call grouping.
- `src/connection.c`: asynchronous DNS/TCP connection attempts.
- `src/config.c`: configuration loading, validation, and atomic saving.
- `include/`: public interfaces and application version.
- `tests/test.c`: regression tests; run with `make test`.

## Author and development

Author and maintainer: **Yuriy_Y** ([alliancesko](https://github.com/alliancesko)).

Developed with the assistance of **OpenAI Codex**, under the author's direction
and supervision. The author defines requirements, guides design decisions, and
tests the utility in their environment; Codex assists with implementation,
review, optimization, and documentation. This is an independent project and is
not an official OpenAI or Asterisk product.

For bug reports, include the astv and Asterisk versions, distribution, terminal
size, reproduction steps, and relevant anonymized AMI responses. Remove secrets,
phone numbers, and other sensitive data before sharing logs or configuration.

## License

Copyright (c) 2026 Yuriy_Y. Distributed under the [MIT License](LICENSE).
