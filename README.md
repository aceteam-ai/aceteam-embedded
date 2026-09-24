# aceteam-embedded

Physical, human-in-the-loop hardware for agentic systems, built on the Flipper
Zero and wired to [aceteam.ai](https://aceteam.ai).

The first thing here is the **AceTeam Beacon**: a pocket device that gates an
agent's risky actions behind a physical press. When an aceteam agent wants to
run a shell command, deploy, spend money, or change the database, it does not
just proceed. It asks the device on your desk, and nothing happens until a human
presses **approve** or **deny**.

This is the "jack in to authorize" model: a hardware presence and approval token
for a deny-by-default agent platform.

> Status: phase 1 (presence). Runs today on stock or Momentum firmware. The
> ladder below states exactly what it proves and what it does not.

## What it proves (read this before calling it "attestation")

It is easy to overclaim here, so the honest ladder:

| Level | Mechanism | What it proves | Status |
|---|---|---|---|
| **L0 Presence** | a human presses OK/Back on the device | a human physically approved/denied on this device, on a link the host already trusts, at time T | **built** |
| **L1 Authentication** | + a user-set PIN entered on the device | the approver knew operator X's PIN (a "something you know" on top of "something you have") | designed |
| **L2 Non-repudiation** | + the device signs the decision with a device-held key | a cryptographic artifact, not just a server log a compromised server could forge | roadmap |
| **L3 Display integrity** | + aceteam signs the action; the device verifies and shows the real action | the human approved the exact action aceteam will execute, closing the "screen shows X, host runs Y" gap | roadmap |

Real "attestation" is roughly **L1 + L2 + L3, which is essentially FIDO2 with
user verification**. The Flipper already ships a U2F/FIDO2 app, so that is the
production-grade path; this repo is the aceteam-branded, legible version you can
read and extend.

What L0 does **not** prove, even with a PIN bolted on naively:

- that the person understood the action,
- that the on-screen text matches what the host actually runs (L3 fixes this),
- that the link was not relayed or the host not compromised (L2 helps),
- non-repudiation (L2).

### On "PIN, user-set, never stored, verified against aceteam"

A user-set PIN is a real upgrade (L1). Two notes so it stays honest:

- "Never stored on the device" is good, but if aceteam verifies the PIN then the
  PIN (or a hash) has to leave the device and cross the host. On a compromised
  host that is capturable. The sovereign version: the PIN unlocks a device-held
  key **locally**, the PIN never leaves the device, and the device proves
  knowledge by signing a challenge. That is L1 + L2 together, and it is where
  this is headed.
- aceteam must still store a verifier (a hash or a public key), so "stored
  nowhere" is not achievable; "not stored on the device, not transmitted in the
  clear" is.

## Architecture

```
agent (aceteam)            host bridge (this repo)          device app (this repo)
  wants to <action>  --->  aceteam_approve <action>  --->   shows action on screen
                              over the Flipper CLI            human presses OK / Back
  proceed / abort    <---  RESULT approve|deny        <---   prints the decision
                           + host writes an audit line
```

- **Device app** (`apps/aceteam_beacon/`, C, built with ufbt): registers a CLI
  command `aceteam_approve <action>`, shows the action, blocks on a physical
  press, replies with a single `RESULT approve|deny|cancelled` line. Handles the
  host disconnecting mid-prompt (fails closed).
- **Host bridge** (`bridge/`, Python): the aceteam side. A mock mode demos it
  standalone (prints "agent wants to X", sends the request, waits for the press,
  writes an audit line, fails closed on timeout). Swap the mock for the real
  aceteam approvals / ask_human path.

Transport is the Flipper's own CLI over USB serial. No custom USB stack, no
firmware fork required. It works on stock firmware.

## Quickstart

Requires a Flipper Zero on USB and
[ufbt](https://github.com/flipperdevices/flipperzero-ufbt).

```bash
# build + run on the connected Flipper
cd apps/aceteam_beacon
ufbt            # build -> dist/aceteam_beacon.fap
ufbt launch     # upload + run it
# it also installs to the SD card: on the Flipper, Apps -> Tools -> AceTeam Beacon
```

Test it by hand from any serial terminal on the Flipper CLI (for example
`tio /dev/ttyACM0`). The app must be running so the command is registered:

```
aceteam_approve deploy prod v1.482
```

The screen shows the action; press OK to approve or Back to deny; the terminal
prints `RESULT approve` (or `deny`). Then run the bridge for the full mock loop:

```bash
pip install -r bridge/requirements.txt
python bridge/aceteam_beacon_bridge.py --port /dev/ttyACM0 --action "deploy prod v1.482"
```

## How this maps to aceteam

aceteam is deny-by-default with a human in the loop (approvals gate, ask_human,
node exec grants). Today those requests fan out to mobile, WhatsApp, and
Telegram. The beacon adds a **physical** surface: a button you press, on a device
you carry, for the highest-risk actions. "The agent literally cannot proceed
until a human presses the button" is the demo.

## Roadmap

- **L1**: PIN entry on the device (user-set), as an authentication factor.
- **L2**: device-held ed25519 key, PIN unlocks it locally, decisions are signed.
- **L3**: aceteam-signed action requests, verified and displayed on the device.
- **Agent-status companion** (`aceteam_status ...`): the device as a pocket
  dashboard of live wave / PR / CI / balance state.
- **Mesh**: Sub-GHz co-signed or quorum approvals across multiple devices.
- **Real aceteam wiring** (approvals / ask_human), replacing the mock bridge.

## License

MIT (see `LICENSE`). Swap if another fits better.
