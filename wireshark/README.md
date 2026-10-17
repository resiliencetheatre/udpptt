# UDPPTT MDPX observer plugin

This is the metadata-proxy native MDPX v1 dissector adapted for UDPPTT, with
UDP and Ethernet Decode As support. It displays cleartext labels and performs
actual libsodium Ed25519 verification over the metadata and complete inner PTT
packet. Encrypted PTT audio stays encrypted; observers need no room password.
Neither ptt_client nor ptt_server uses this verification as an admission policy.

## Build and install

Build against the Wireshark major/minor version that will load the plugin.
Debian build dependencies include `libwireshark-dev`, `libsodium-dev`,
`pkg-config` and a C compiler; offline tests also require `tshark` and Python 3.

```sh
make wireshark
make test-wireshark
tshark -G folders
```

Close Wireshark, then copy `wireshark/mdpx.so` into the `epan` subdirectory of
the **Personal Plugins** directory reported by TShark (the binary plugin folder,
not Personal Lua Plugins). Reopen Wireshark. Install only one MDPX plugin:
this replaces, rather than accompanies, metadata-proxy's `mdpx.so`.
No installation is performed by the build or tests.

## Trust and decoding

Create `observer-trust.txt` from the sender's public key:

```sh
awk '{print $0 " HOST-A"}' dpi.public > observer-trust.txt
```

Provision that public-key/label association through a trusted channel.
In Preferences → Protocols → MDPX, set **Public-key trust file** to its absolute
path. Reopen the capture or restart live capture after changing the file.

Automatic UDP binding defaults to disabled (`mdpx.udp_port:0`). Select
**Decode As → UDP port → MDPX** for the PTT port (normally 5000), or set the
UDP port preference. For Blackfiber, use **Decode As → EtherType → MDPX**
(normally 0x88b5). Ordinary unsigned PTT packets decoded as MDPX are reported
as malformed MDPX; this does not mean the underlying PTT packet is malformed.

```sh
tshark -r capture.pcapng -d udp.port==5000,mdpx \
  -o "mdpx.trust_file:$PWD/observer-trust.txt" \
  -T fields -e mdpx.label -e mdpx.signature_valid -e mdpx.authorized

# Blackfiber capture:
tshark -r blackfiber.pcapng -d ethertype==0x88b5,mdpx \
  -o "mdpx.trust_file:$PWD/observer-trust.txt"
```

Useful display filters:

```text
mdpx.label == "HOST-A"
mdpx.signature_valid == true && mdpx.authorized == true
mdpx.signature_valid == false
mdpx.known_key == false
mdpx.capture_complete == false
```

Missing verification fields mean **not checked**, not successful verification.
The plugin displays key ID, timestamp, session, sequence, label, signature and
inner payload. Status codes are 0 valid/authorized, 1 invalid signature,
2 unknown key, 3 valid signature/unauthorized label, 4 malformed structure,
5 incomplete capture, 6 no trust file, 7 invalid trust configuration, and
8 cryptographic initialization failure.

Capture the entire packet: the signature covers the encrypted bytes too.
Verification proves endorsement by the trusted signing-key holder; it does not
bind outer source addresses, prove physical origin, enforce freshness, or detect
replays. Verification is recomputed on redissection. The offline tests use an
isolated temporary plugin/configuration directory and do not install anything.
See [the wire format](../dpi/PROTOCOL.md).
