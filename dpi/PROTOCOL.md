# UDPPTT observer metadata: MDPX v1

## Wire protocol v1 (normative)

Every outer UDP payload has this exact layout. All numeric fields are unsigned
big-endian integers. Offsets are zero based. No optional fields or padding.

| Offset | Size | Field | Rule |
| --- | --- | --- | --- |
| 0 | 4 | magic | ASCII MDPX |
| 4 | 1 | version | 1 |
| 5 | 1 | flags | 0; reject any other value |
| 6 | 2 | header_length | 115 + label_length, including signature |
| 8 | 1 | label_length | 1..63 |
| 9 | 2 | payload_length | exact original UDP payload length |
| 11 | 8 | key_id | first 8 bytes of SHA-256(raw 32-byte public key) |
| 19 | 8 | timestamp_ms | Unix epoch milliseconds from CLOCK_REALTIME |
| 27 | 16 | session_id | fresh cryptographic random bytes per sender start |
| 43 | 8 | sequence | starts at 0; increment per generated packet |
| 51 | L | label | L ASCII bytes; no terminating NUL |
| 51+L | 64 | signature | detached Ed25519 signature |
| 115+L | P | original_payload | P unmodified bytes |

Labels permit only [A-Za-z0-9._-], with case-sensitive authorization. HOST-A
is six bytes, giving a 121-byte wrapper. Restrict labels before signing and
while parsing. Reject unknown versions, reserved flags, invalid labels,
inconsistent lengths, truncation, and trailing bytes. Perform overflow-safe
length calculations before accessing fields. Zero-length payloads are valid.

The exact Ed25519 message is:

    ASCII("metadata-proxy/v1") || packet[0 : 51+L] || original_payload

The ASCII domain prefix is exactly 17 bytes, without a NUL. Slice notation is
half-open. The signature field is excluded. Use libsodium's standard
crypto_sign_detached()/crypto_sign_verify_detached() (Pure Ed25519), not
Ed25519ph, a custom prehash, or a truncated signature. Construct the message
in a reusable bounded buffer. A signature covers all metadata and payload.
Use sodium_init(), randombytes_buf(), and the library's primitives.
Abort/restart the session before sequence wrap; never reuse a session after
restart. A send failure may leave sequence gaps; that is acceptable.

## UDPPTT processing

The inner payload is exactly one existing UPTT v2 packet, including its header
and encrypted payload/tag when encryption is enabled. UDPPTT limits the outer
packet to 2048 bytes, and the sending transport may impose a smaller MTU limit.
The MDPX codec itself supports the reference maximum of 65507 bytes.

UDPPTT receivers and the relay validate wrapper structure and the inner PTT
packet only. They do not verify the signature, require a trusted key, check
wall-clock freshness, or maintain an MDPX replay window. Unsigned UPTT v2 packets
remain accepted. Existing PTT arbitration, jitter and encryption rules apply.
The relay preserves the entire signed datagram when forwarding.

Observers verify the signature and the label's authorization using a trusted
public-key file containing `PUBLIC_KEY_HEX LABEL` lines. Blank lines and whole-line
`#` comments are accepted. The key ID is only a lookup hint. The public key is
not transmitted in the wrapper and must be provisioned through a trusted channel.
The Wireshark plugin is stateless: a valid signature on a historical or repeated
packet remains valid. It does not establish freshness, physical origin, or the
observed outer IP/MAC address. Those addresses are outside the signature.
