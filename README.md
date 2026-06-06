# astv

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
third-party runtime dependencies.

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
port 5038 to the public Internet. Prefer a VPN, an SSH tunnel, or AMI TLS.

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
ASTV_SECRET='replace-with-a-long-random-secret' ./astv \
  --host 127.0.0.1 --user astv
```

Using a protected file:

```sh
printf '%s\n' 'replace-with-a-long-random-secret' > ~/.astv-secret
chmod 600 ~/.astv-secret
./astv --host 127.0.0.1 --user astv --secret-file ~/.astv-secret
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
