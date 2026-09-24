# Roadmap

AceTeam Beacon: physical, human-in-the-loop approval for agentic systems. The
guiding frame is the trust ladder from the [README](README.md#what-it-proves-read-this-before-calling-it-attestation):
each milestone is a rung, and we do not claim a rung we have not built.

## Status

- [x] **M0 - Presence (L0).** `SHIPPED.` Device app gates an agent action behind
      a physical OK/Back press over the Flipper CLI. Fails closed on host
      disconnect. Mock host bridge. No-hardware end-to-end mock demo.

## Next

- [ ] **M1 - Authentication (L1): PIN.** A user-set PIN entered on the device as
      an approval factor. Design constraint: the PIN unlocks a device-held key
      *locally* and never leaves the device; aceteam verifies a signature, not
      the PIN. (Naive "send the PIN to aceteam" is rejected: capturable on a
      compromised host.)
- [ ] **M2 - Non-repudiation (L2): device signing key.** Each device holds an
      ed25519 keypair (PIN-unlocked). Decisions are signed, so the audit record
      is a cryptographic artifact, not a forgeable server log. Follows aceteam's
      provenance conventions: sign a canonical record root, trust a
      caller-supplied identity resolver, never a key riding on the payload.
- [ ] **M3 - Display integrity (L3): aceteam-signed actions.** aceteam signs the
      action request; the device verifies and shows the *real* action, closing
      the "screen shows X, host runs Y" gap. This is the rung that makes the
      human's press actually mean something.

## Beyond

- [ ] **M4 - Agent-status companion.** `aceteam_status ...` mode: the device as a
      pocket dashboard of live wave / PR / CI / balance state.
- [ ] **M5 - Mesh.** Sub-GHz co-signed or quorum approvals across multiple
      devices ("2 of 3 operators present"). Scoped to sit beside, not collide
      with, any network-layer egress-attestation effort.
- [ ] **M6 - Real aceteam wiring.** Replace the mock bridge with the live
      approvals / ask_human path; the beacon becomes a physical surface for
      existing gated actions.

## Non-goals (for now)

- Crypto cold-storage / secure-element key custody. The Flipper has no secure
  element; this is presence and integrity, not tamper-proof custody.
- Proving a human *understood* an action, or that a model has no bad intent. We
  prove who pressed, when, on which device, over what the host displayed.
  Nothing more, and the docs say so.

## How to assess it today

- **No hardware:** `python3 demo/mock_e2e.py` mocks the agent, the device, and
  the transport, and walks the approve / deny / fail-closed paths with an audit
  trail.
- **With a Flipper:** build and `ufbt launch` the app, then drive
  `aceteam_approve` from a serial terminal or the host bridge (see the README).
