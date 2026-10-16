# Security ladder design: M1 (PIN), M2 (device signing), M3 (display integrity)

Status: **design for review, not an implementation.** This document proposes the
protocol, record shapes, and key lifecycle for the three security rungs
(issues #2, #3, #4). It is meant to be read, argued with, and then locked by
Jason on the issues. Nothing here is built yet, and no crypto code should be
written until the decisions in the summary table are locked.

The guiding rule from the README still holds: we do not claim a rung we have not
built, and we state exactly what each rung proves and does not prove.

## 0. Why this document leads with feasibility

Issues #3 and #4 assume the device can do public-key signing and verification.
Before designing the protocol, the first job was to check what the device's
firmware actually gives an external app. The answer reshapes the design, so it
comes first.

## 1. Feasibility ground truth (mntm-012, API 87.1)

Checked against the live SDK on the dev host (`api_symbols.csv` and the
`furi_hal` headers). In that file, a symbol marked `+` is callable from an
external `.fap`; a symbol marked `-` is compiled into the firmware but **not**
exported to apps.

**App-callable crypto (the whole list):**

| Capability | Symbols | Notes |
|---|---|---|
| AES engine (ECB, CTR, GCM) | `furi_hal_crypto_encrypt/decrypt/ctr/gcm*` (`+`) | symmetric only |
| Secure enclave key slots | `furi_hal_crypto_enclave_store_key/load_key/unload_key/ensure_key/verify` (`+`) | stores AES keys on core2/FUS |
| Hardware TRNG | `furi_hal_random_fill_buf/get/init` (`+`) | STM32WB RNG, good for keygen and nonces |
| MD5 of a file | `md5_calc_file` (`+`) | the only app-exported hash, and MD5, so useless for signing |

**Present in firmware but NOT app-callable (all marked `-`):** every `mbedtls_*`
function, including `mbedtls_sha256`, `mbedtls_ecdsa_sign`, `mbedtls_ecdsa_verify`,
and the whole `mbedtls_ecp_*` family. There is **no ed25519 or curve25519 anywhere**
in the SDK (mbedtls here is built without it).

**Consequences that drive the design:**

1. There is **no app-callable public-key primitive and no app-callable
   cryptographic hash** (SHA-256/512). So M2 (device signing) and M3 (verifying
   aceteam's signature) cannot use the firmware's crypto. They require a
   **self-contained crypto library vendored into the `.fap`**.
2. The recommendation is to vendor one small, well-reviewed, public-domain
   implementation rather than write any crypto. **TweetNaCl** is the proposed
   choice: a single C file that provides ed25519 (`crypto_sign` /
   `crypto_sign_open`) and SHA-512 (`crypto_hash`), which covers both signing and
   the record-root hash. It is pinned in the repo by SHA-256 and never edited.
3. The hardware TRNG (`furi_hal_random_fill_buf`) is real and should seed all key
   generation and all nonces. Do not reseed a software PRNG from a weak source.
4. The AES secure enclave is useful for wrapping the private key at rest (see M1),
   but it is symmetric, so it cannot itself provide non-repudiation.

**Honesty, quoted from the firmware's own `furi_hal_crypto.h`:** "Flipper was
never designed to be secure, nor did it pass a cryptography audit... there is no
way to securely deliver user keys to the device and never will be... the device
is fully open and can be easily dumped with a debugger or modified code." The
enclave runs on core2 (FUS) and locks the AES key registers while a key is
loaded, which raises the bar against a pure flash dump, but it is not an HSM and
the docs say so. Our custody story must stay inside those limits.

## 2. Cross-cutting decisions (these gate all three rungs)

Each has a recommendation; the decision is Jason's.

- **D1. Crypto provider.** Vendor TweetNaCl (ed25519 + SHA-512) into
  `apps/aceteam_beacon/vendor/`, pinned by SHA-256, with its provenance recorded.
  *Recommend: yes.* Alternative: hand-port a different reviewed library. Rejected
  alternative: roll our own (never).
- **D2. Signature scheme.** ed25519 over a SHA-512 canonical record root.
  *Recommend: ed25519*, which matches issue #3 and is the smallest, simplest
  deterministic option. (ECDSA P-256 would also have to be vendored and needs
  careful per-signature nonces; ed25519 avoids that class of bug.)
- **D3. Symmetric alternative for M2 is rejected.** An HMAC or enclave-AES "signature"
  would force aceteam to hold the same secret, so aceteam could forge the device's
  decision. That defeats the explicit non-repudiation goal of #3. Symmetric crypto
  is used only for key-at-rest wrapping (M1), never as the decision signature.
- **D4. Private key at rest** is wrapped by **both** a PIN-derived key **and** the
  enclave device-unique key (slot 11). *Recommend: both* (details in M1).
- **D5. Identity resolver.** aceteam maps a device id to an enrolled public key on
  its side (caller-supplied resolver). The device id asserted on the wire is only a
  lookup key; trust comes from the signature verifying against the enrolled public
  key, never from the asserted id. A device id for an unknown key fails closed.
- **D6. Canonical root, not raw bytes.** Both the device decision (M2) and the
  aceteam request (M3) are signed as a domain-separated, fixed-layout record root
  (a SHA-512 over a versioned, fixed-width serialization), never over
  ambiguous free text.

## 3. M1: device-entered PIN (issue #2)

**Goal.** Add "something you know" on top of "something you have": a PIN, set and
entered on the device, that unlocks the device-held signing key locally. The PIN
never leaves the device, and aceteam stores only a public verifier, never the PIN
or a PIN hash.

**UX.**
- First run: the device generates the signing keypair (M2) from the TRNG and asks
  the operator to set a PIN, entered twice to confirm. A 6-digit PIN is the default
  (see the entropy note); the length is a locked parameter.
- Per decision: when an approval needs a signature, the device prompts for the PIN,
  unwraps the private key into RAM for exactly that one signing operation, then
  zeroizes it. A wrong PIN never produces a signature.

**Key lifecycle and storage.**
- Stored on SD at `/ext/apps_data/aceteam_beacon/`: a random 16-byte salt, a KDF
  iteration count, a failure counter, and the double-wrapped private key blob. The
  public key is stored in the clear (it is public).
- Wrapping, inner to outer:
  1. `kek = PBKDF2-HMAC-SHA512(PIN, salt, iters)` using the vendored SHA-512. The
     iteration count is tuned to roughly 0.5 to 1 second on the device.
  2. `inner = AES-GCM(key = kek, privkey)`.
  3. `blob = AES-GCM_enclave(slot 11, inner)` using the device-unique enclave key.
- Unlock therefore needs **both** the PIN (to derive `kek`) **and** this physical
  device (to run the enclave). A flash dump alone cannot unwrap it, because the
  enclave key lives on core2 and never leaves it.

**Retry and lockout.** The failure counter is stored inside an authenticated
(GCM) record so it cannot be silently edited, and it drives an escalating delay
after each wrong PIN. An optional, operator-chosen policy wipes the wrapped key
after M consecutive failures. Honest limit: an attacker who can restore a full
flash image can roll the counter back, so lockout is a strong speed bump, not a
hardware guarantee.

**Threat model, including the known forge.** A second input channel (BLE RPC
`input send`) can inject button presses into the GUI, so a bare presence press is
forgeable. M1 is the first real mitigation: a forged OK with no PIN yields no
valid signature, and once M2 lands aceteam must require the signed decision and
reject a bare `RESULT approve` (see D6 and the M2 migration). The residual risk is
an attacker who both injects input and already knows the PIN; the lockout and the
preference for physical-only input during PIN entry are the mitigations, and we
state plainly that a device exposing a remote input channel needs that channel
authenticated.

**Proves / does not prove.** Proves: the approver knew the PIN that unlocks this
specific device's key. Does not prove: that the host is uncompromised, that the
person understood the action (no rung proves that), non-repudiation (M2), or that
the screen matched what the host runs (M3).

**Open decisions for #2:** PIN length and whether alphanumeric is allowed; KDF
iteration target; wipe-after-M-failures on or off and the value of M; whether PIN
entry is restricted to physical input only.

## 4. M2: device signing key (issue #3)

**Goal.** Each device holds an ed25519 keypair (PIN-unlocked per M1) and signs
every decision, so the audit record is a cryptographic artifact rather than a
server log a compromised server could forge.

**Canonical decision record (the thing that is signed).** Fixed layout, fixed
widths, big-endian, no delimiters, with a domain-separation prefix:

```
domain  = "aceteam-beacon/decision/v1\0"   (ASCII, NUL-terminated)
fields  = version      : u8   = 1
          device_id    : 16 bytes   (first 16 bytes of SHA-512(public key))
          action_hash  : 32 bytes   (first 32 bytes of SHA-512(action text), from M3's request)
          nonce        : 16 bytes   (echoed from aceteam's request, anti-replay)
          timestamp    : u64        (device clock, unix seconds; see clock caveat)
          decision     : u8         (0 = deny, 1 = approve, 2 = cancelled)
root    = SHA-512(domain || fields)        (64 bytes, the "record root")
sig     = ed25519_sign(privkey, root)      (64 bytes)
```

Signing the root, not raw CLI text, is deliberate (D6): the verifier rebuilds the
record from structured fields, so there is no parsing ambiguity to exploit.

**Transport (replaces M0's bare `RESULT approve`).** One line:

```
RESULT <decision> device=<device_id_hex> nonce=<nonce_hex> ts=<unix> ed25519=<sig_base64>
```

**Enrollment (out of band).** On first run the device generates the keypair from
the TRNG and exposes the public key over a new read-only CLI command
(`aceteam_pubkey`, printed as hex and optionally a QR on screen). The operator
enrolls `device_id -> public key` with aceteam once, out of band. The private key
never leaves the device and never rides on any payload.

**Host-bridge verification and audit.** The bridge receives the RESULT line,
rebuilds `root` from the `action_hash` it sent plus the returned
`device_id/nonce/timestamp/decision`, looks up the enrolled public key for
`device_id` (the caller-supplied resolver, D5), and verifies the signature. It
records `verified: true|false` in the audit line and **fails closed** on a bad
signature, an unknown device id, a stale timestamp, or a nonce it did not issue.
Only `decision == approve` with `verified == true` proceeds.

**Proves / does not prove.** Proves: this specific enrolled device produced this
decision over this exact action and nonce, and the server cannot forge it. Does
not prove: tamper-proof key custody (the key is soft, see section 1), or that the
operator understood the action.

**Open decisions for #3:** the exact field widths and order above (lock them);
whether `device_id` is the public-key fingerprint (recommended) or the hardware
UID; the clock-trust stance (recommend aceteam's nonce as the real freshness
guarantee and treat the device timestamp as advisory); the RESULT line grammar.

## 5. M3: aceteam-signed actions, display integrity (issue #4)

**Goal.** Close the "screen shows deploy staging, host runs rm -rf prod" gap.
aceteam signs the action request; the device verifies it against a pinned aceteam
public key and renders only the verified action text. An unverified or tampered
request is refused, not shown.

**aceteam request envelope (the thing aceteam signs).**

```
domain  = "aceteam-beacon/request/v1\0"
fields  = version      : u8   = 1
          key_id       : u8          (which pinned aceteam key signed this)
          request_id   : 16 bytes    (random; becomes the decision nonce)
          issued_at    : u64
          expires_at   : u64
          action_len   : u16
          action_text  : action_len bytes (UTF-8, shown to the human)
root    = SHA-512(domain || fields)
sig     = ed25519_sign(aceteam_privkey, root)
```

Wire: `aceteam_approve <base64(fields || sig)>`. The device base64-decodes, finds
`key_id` in its pinned aceteam key set, verifies the signature over the rebuilt
root, and checks `expires_at`. Only then does it display `action_text`. On any
failure it shows "unverified request refused" and renders no action text.

The device's decision record (M2) sets `action_hash = SHA-512(action_text)[:32]`
and `nonce = request_id`, binding the human's signed decision to the exact bytes
that were verified and displayed.

**aceteam key pinning and rotation.** The device holds a small set of aceteam
public keys indexed by `key_id`. A compiled-in default key ships with the app;
additional keys are enrolled out of band via a config file on SD. Rotation is a
signed message from an already-pinned key authorizing a new `key_id`, so trust
chains from the existing pin. Whether first-use trust (TOFU) is ever allowed is a
locked decision; the safe default is no TOFU, enroll explicitly.

**How M1 + M2 + M3 approximate FIDO2 with user verification, honestly.** M1 is the
user-verification factor (PIN), M2 is the device-bound private key doing a
challenge-response signature (the aceteam nonce is the challenge), and M3 is the
relying party (aceteam) signing the request that the authenticator displays and
verifies. That is the shape of WebAuthn with UV. The honest differences: there is
no secure element and no attestation certificate, so key custody is soft and
dumpable; there is no authenticator-model attestation; and freshness rests on the
aceteam nonce rather than hardware. So "essentially FIDO2 with UV" is true about
the protocol shape and false about the custody guarantee. We say both.

**Proves / does not prove.** Proves: the human approved the exact action aceteam
signed and the device displayed, and a host that altered the action text is caught
(the signature fails). Does not prove: that aceteam's own signing key is uncompromised
(that is aceteam's custody problem), or tamper-proof device custody.

**Open decisions for #4:** the envelope layout above; compiled-in default key plus
SD enrollment vs SD-only; TOFU allowed or not; the rotation-message format;
`expires_at` default window.

## 6. Non-goals (carried from the README, restated so the design stays honest)

- No secure-element key custody. The Flipper has none; the enclave raises the bar
  against a flash dump but not against on-device code or a debugger.
- We do not prove a human understood the action, or that a model has no bad intent.
- A low-entropy PIN wrapped on a fully dumpable device is brute-forceable offline
  if the attacker also defeats the enclave layer; the iteration count and lockout
  raise cost, they do not make it impossible.

## 7. Decisions for Jason to lock

| # | Decision | Recommendation |
|---|---|---|
| D1 | Vendor TweetNaCl (ed25519 + SHA-512) into the fap, pinned by hash | Yes |
| D2 | Signature scheme | ed25519 over a SHA-512 root |
| D3 | Symmetric "signature" for M2 | Rejected; public-key only |
| D4 | Private key at rest: PIN-KDF wrap plus enclave wrap | Both layers |
| D5 | Identity resolver: aceteam maps device_id to enrolled pubkey | Yes, fail closed on unknown |
| D6 | Sign a canonical root, not raw text | Yes |
| M1 | PIN length, KDF iters, wipe-after-M, physical-only entry | 6 digits, ~0.5 to 1s KDF, wipe off by default, physical-only |
| M2 | Decision record layout, device_id source, clock stance, RESULT grammar | As specified; device_id = pubkey fingerprint; nonce is the real freshness |
| M3 | Envelope layout, key pinning, TOFU, rotation, expiry window | As specified; compiled default + SD enroll; no TOFU |
| D6b | Deprecate bare `RESULT approve`: aceteam rejects unsigned decisions once M2 ships | Yes, fail closed |

Once these are locked on #2, #3, #4, the implementation order is M2 and M1
together (keygen, storage, PIN, signing, bridge verify), then M3 (envelope,
pinning, verify-before-display). Each lands as its own slice with the honest
README ladder updated to match what is actually built.
