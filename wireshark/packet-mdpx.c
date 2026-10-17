/* SPDX-License-Identifier: GPL-2.0-or-later
 * Stateless MDPX v1 dissection and Pure Ed25519 verification.
 * The shared MDPX protocol and observer trust parsers are compiled into this plugin.
 */
#include <epan/packet.h>
#include <epan/expert.h>
#include <epan/prefs.h>
#include <ws_version.h>
#include <wsutil/plugins.h>
#include <sodium.h>
#include <string.h>
#include "mdpx.h"

WS_DLL_PUBLIC_DEF const char plugin_version[] = "1.0.0";
WS_DLL_PUBLIC_DEF const int plugin_want_major = WIRESHARK_VERSION_MAJOR;
WS_DLL_PUBLIC_DEF const int plugin_want_minor = WIRESHARK_VERSION_MINOR;

static int proto_mdpx = -1;
static int ett_mdpx = -1;
static int hf_magic = -1, hf_version = -1, hf_flags = -1;
static int hf_header_length = -1, hf_label_length = -1, hf_payload_length = -1;
static int hf_key_id = -1, hf_timestamp = -1, hf_session = -1, hf_sequence = -1;
static int hf_label = -1, hf_signature = -1, hf_payload = -1;
static int hf_status = -1, hf_complete = -1, hf_structure = -1;
static int hf_known = -1, hf_authorized = -1, hf_valid = -1;
static expert_field ei_malformed = EI_INIT;
static expert_field ei_incomplete = EI_INIT;
static expert_field ei_unverified = EI_INIT;
static expert_field ei_configuration = EI_INIT;
static expert_field ei_signature = EI_INIT;
static expert_field ei_authorization = EI_INIT;

/* No private keys, replay state or frame-result cache. Trust reloads on a new
 * capture or preference application. Wireshark serializes dissector calls. */
static struct trust trusted;
static struct workspace scratch;
static const char *trust_path = "";
static unsigned udp_port = 0, installed_port;
static bool crypto_ready;
static dissector_handle_t mdpx_handle;
static enum { TRUST_NONE, TRUST_READY, TRUST_ERROR } trust_state;

enum observation {
    OBS_VALID = 0, OBS_INVALID_SIGNATURE = 1, OBS_UNKNOWN_KEY = 2,
    OBS_UNAUTHORIZED = 3, OBS_MALFORMED = 4, OBS_INCOMPLETE = 5,
    OBS_NO_TRUST = 6, OBS_TRUST_ERROR = 7, OBS_CRYPTO_ERROR = 8
};
static const value_string statuses[] = {
    { OBS_VALID, "Valid signature; authorized label" },
    { OBS_INVALID_SIGNATURE, "Invalid Ed25519 signature" },
    { OBS_UNKNOWN_KEY, "Unknown key; signature not checked" },
    { OBS_UNAUTHORIZED, "Valid signature; unauthorized label" },
    { OBS_MALFORMED, "Malformed MDPX packet; signature not checked" },
    { OBS_INCOMPLETE, "Incomplete capture; signature not checked" },
    { OBS_NO_TRUST, "No trust file configured; signature not checked" },
    { OBS_TRUST_ERROR, "Trust configuration error; signature not checked" },
    { OBS_CRYPTO_ERROR, "Cryptographic initialization failed; signature not checked" },
    { 0, NULL }
};

static void load_trust(void)
{
    /* Discard the entire old table even if the new file only partly parses. */
    memset(&trusted, 0, sizeof trusted);
    trust_state = TRUST_NONE;
    if (trust_path && *trust_path) {
        if (trust_load(trust_path, &trusted)) {
            memset(&trusted, 0, sizeof trusted);
            trust_state = TRUST_ERROR;
        } else {
            trust_state = TRUST_READY;
        }
    }
}

static void generated_bool(proto_tree *tree, int field, tvbuff_t *tvb, bool value)
{
    proto_item *item = proto_tree_add_boolean(tree, field, tvb, 0, 0, value);
    proto_item_set_generated(item);
}

static void outcome(proto_tree *tree, proto_item *root, tvbuff_t *tvb,
                    packet_info *pinfo, enum observation status)
{
    const char *description = val_to_str_const(status, statuses, "Unknown result");
    proto_item *item = proto_tree_add_uint(tree, hf_status, tvb, 0, 0, status);
    proto_item_set_generated(item);
    col_append_fstr(pinfo->cinfo, COL_INFO, "%s", description);
    proto_item_append_text(root, ": %s", description);
    expert_field *expert = NULL;
    switch (status) {
    case OBS_MALFORMED: expert = &ei_malformed; break;
    case OBS_INCOMPLETE: expert = &ei_incomplete; break;
    case OBS_INVALID_SIGNATURE: expert = &ei_signature; break;
    case OBS_UNAUTHORIZED: expert = &ei_authorization; break;
    case OBS_TRUST_ERROR: case OBS_CRYPTO_ERROR: expert = &ei_configuration; break;
    case OBS_NO_TRUST: case OBS_UNKNOWN_KEY: expert = &ei_unverified; break;
    case OBS_VALID: break;
    }
    if (expert) expert_add_info(pinfo, item, expert);
}

/* Display only fields actually captured, even when the packet is malformed.
 * Displayed metadata must not be mistaken for authenticated metadata. */
static void fixed_fields(proto_tree *tree, tvbuff_t *tvb, unsigned captured)
{
    struct { int field; unsigned offset, size; unsigned encoding; } fields[] = {
        { hf_magic, 0, 4, ENC_ASCII },
        { hf_version, 4, 1, ENC_BIG_ENDIAN },
        { hf_flags, 5, 1, ENC_BIG_ENDIAN },
        { hf_header_length, 6, 2, ENC_BIG_ENDIAN },
        { hf_label_length, 8, 1, ENC_BIG_ENDIAN },
        { hf_payload_length, 9, 2, ENC_BIG_ENDIAN },
        { hf_key_id, 11, 8, ENC_NA },
        { hf_timestamp, 19, 8, ENC_BIG_ENDIAN },
        { hf_session, 27, 16, ENC_NA },
        { hf_sequence, 43, 8, ENC_BIG_ENDIAN }
    };
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        if (captured >= fields[i].offset + fields[i].size)
            proto_tree_add_item(tree, fields[i].field, tvb,
                                (int)fields[i].offset, (int)fields[i].size,
                                fields[i].encoding);
    }
    if (captured >= 9) {
        unsigned length = tvb_get_uint8(tvb, 8);
        if (length >= 1 && length <= 63 && captured >= 51 + length)
            proto_tree_add_item(tree, hf_label, tvb, 51, (int)length, ENC_ASCII);
    }
}

static int dissect_mdpx(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree,
                        void *data)
{
    (void)data;
    unsigned captured = tvb_captured_length(tvb);
    unsigned reported = tvb_reported_length(tvb);
    col_set_str(pinfo->cinfo, COL_PROTOCOL, "MDPX");
    col_clear(pinfo->cinfo, COL_INFO);
    proto_item *root = proto_tree_add_item(tree, proto_mdpx, tvb, 0, -1, ENC_NA);
    proto_tree *mdpx = proto_item_add_subtree(root, ett_mdpx);
    generated_bool(mdpx, hf_complete, tvb, captured >= reported);
    fixed_fields(mdpx, tvb, captured < reported ? captured : reported);
    if (captured < reported) {
        outcome(mdpx, root, tvb, pinfo, OBS_INCOMPLETE);
        return (int)captured;
    }
    struct packet packet;
    /* tvb_get_ptr is called only after the bounded length check. */
    if (captured != reported || captured < 116 || captured > CAP ||
        decode(tvb_get_ptr(tvb, 0, (int)captured), captured, &packet)) {
        generated_bool(mdpx, hf_structure, tvb, false);
        outcome(mdpx, root, tvb, pinfo, OBS_MALFORMED);
        return (int)captured;
    }
    generated_bool(mdpx, hf_structure, tvb, true);
    proto_tree_add_item(mdpx, hf_signature, tvb, 51 + (int)packet.label_length,
                        64, ENC_NA);
    proto_tree_add_item(mdpx, hf_payload, tvb, 115 + (int)packet.label_length,
                        (int)packet.payload_length, ENC_NA);
    col_add_fstr(pinfo->cinfo, COL_INFO, "%s seq=%" G_GUINT64_FORMAT " len=%zu: ",
                 packet.label, (guint64)packet.sequence, packet.payload_length);
    if (!crypto_ready) {
        outcome(mdpx, root, tvb, pinfo, OBS_CRYPTO_ERROR);
        return (int)captured;
    }
    if (trust_state != TRUST_READY) {
        outcome(mdpx, root, tvb, pinfo,
                trust_state == TRUST_ERROR ? OBS_TRUST_ERROR : OBS_NO_TRUST);
        return (int)captured;
    }
    const struct identity *key = NULL;
    bool authorized = false;
    for (size_t i = 0; i < trusted.count; i++) {
        if (!memcmp(packet.id, trusted.entries[i].id, 8)) {
            key = &trusted.entries[i];
            if (!strcmp(packet.label, key->label)) authorized = true;
        }
    }
    generated_bool(mdpx, hf_known, tvb, key != NULL);
    if (!key) {
        outcome(mdpx, root, tvb, pinfo, OBS_UNKNOWN_KEY);
        return (int)captured;
    }
    generated_bool(mdpx, hf_authorized, tvb, authorized);
    /* Check signatures even for a known key with an unauthorized label. */
    bool valid = verify(&scratch, &packet, key->pk) == 0;
    generated_bool(mdpx, hf_valid, tvb, valid);
    outcome(mdpx, root, tvb, pinfo, !valid ? OBS_INVALID_SIGNATURE :
            authorized ? OBS_VALID : OBS_UNAUTHORIZED);
    return (int)captured;
}

static void handoff_mdpx(void)
{
    if (!mdpx_handle) {
        mdpx_handle = register_dissector("mdpx", dissect_mdpx, proto_mdpx);
        dissector_add_for_decode_as("udp.port", mdpx_handle);
        dissector_add_for_decode_as("ethertype", mdpx_handle);
    } else if (installed_port) {
        dissector_delete_uint("udp.port", installed_port, mdpx_handle);
    }
    installed_port = udp_port <= 65535 ? udp_port : 0;
    if (installed_port) dissector_add_uint("udp.port", installed_port, mdpx_handle);
    load_trust();
}

static void register_mdpx(void)
{
#define FIELD(id, title, name, type, base, desc) \
    { &id, { title, "mdpx." name, type, base, NULL, 0, desc, HFILL } }
    static hf_register_info fields[] = {
        FIELD(hf_magic, "Magic", "magic", FT_STRING, BASE_NONE, NULL),
        FIELD(hf_version, "Version", "version", FT_UINT8, BASE_DEC, NULL),
        FIELD(hf_flags, "Flags", "flags", FT_UINT8, BASE_HEX, NULL),
        FIELD(hf_header_length, "Header length", "header_length", FT_UINT16, BASE_DEC, NULL),
        FIELD(hf_label_length, "Label length", "label_length", FT_UINT8, BASE_DEC, NULL),
        FIELD(hf_payload_length, "Original payload length", "payload_length", FT_UINT16, BASE_DEC, NULL),
        FIELD(hf_key_id, "Key ID", "key_id", FT_BYTES, BASE_NONE, NULL),
        FIELD(hf_timestamp, "Timestamp (Unix milliseconds)", "timestamp_ms", FT_UINT64, BASE_DEC, NULL),
        FIELD(hf_session, "Session ID", "session_id", FT_BYTES, BASE_NONE, NULL),
        FIELD(hf_sequence, "Sequence", "sequence", FT_UINT64, BASE_DEC, NULL),
        FIELD(hf_label, "Label", "label", FT_STRING, BASE_NONE, "Unverified until signature and authorization checks succeed"),
        FIELD(hf_signature, "Ed25519 signature", "signature", FT_BYTES, BASE_NONE, NULL),
        FIELD(hf_payload, "Original payload", "payload", FT_BYTES, BASE_NONE, NULL),
        FIELD(hf_complete, "Complete capture", "capture_complete", FT_BOOLEAN, BASE_NONE, NULL),
        FIELD(hf_structure, "Structure valid", "structure_valid", FT_BOOLEAN, BASE_NONE, NULL),
        FIELD(hf_known, "Trusted key found", "known_key", FT_BOOLEAN, BASE_NONE, NULL),
        FIELD(hf_authorized, "Label authorized for key", "authorized", FT_BOOLEAN, BASE_NONE, "Authorization mapping; check signature_valid separately"),
        FIELD(hf_valid, "Ed25519 signature valid", "signature_valid", FT_BOOLEAN, BASE_NONE, "Absent when verification was not performed; no freshness or replay claim"),
        { &hf_status, { "Verification result", "mdpx.status", FT_UINT8, BASE_DEC,
                        VALS(statuses), 0, NULL, HFILL } }
    };
#undef FIELD
    static int *subtrees[] = { &ett_mdpx };
    static ei_register_info experts[] = {
        { &ei_malformed, { "mdpx.malformed", PI_MALFORMED, PI_ERROR,
            "Malformed MDPX v1 structure; signature not checked", EXPFILL } },
        { &ei_incomplete, { "mdpx.incomplete", PI_UNDECODED, PI_WARN,
            "Capture does not contain the complete UDP payload; signature not checked", EXPFILL } },
        { &ei_unverified, { "mdpx.unverified", PI_SECURITY, PI_NOTE,
            "No configured trusted key available; signature not checked", EXPFILL } },
        { &ei_configuration, { "mdpx.configuration_error", PI_SECURITY, PI_ERROR,
            "Cannot verify: check trust-file path/contents or crypto initialization", EXPFILL } },
        { &ei_signature, { "mdpx.invalid_signature", PI_SECURITY, PI_ERROR,
            "Ed25519 verification failed for the configured trusted key", EXPFILL } },
        { &ei_authorization, { "mdpx.unauthorized_label", PI_SECURITY, PI_WARN,
            "Signature valid but this key is not authorized for the label", EXPFILL } }
    };
    crypto_ready = sodium_init() >= 0;
    proto_mdpx = proto_register_protocol("Metadata Proxy", "MDPX", "mdpx");
    proto_register_field_array(proto_mdpx, fields, G_N_ELEMENTS(fields));
    proto_register_subtree_array(subtrees, G_N_ELEMENTS(subtrees));
    expert_module_t *expert = expert_register_protocol(proto_mdpx);
    expert_register_field_array(expert, experts, G_N_ELEMENTS(experts));
    module_t *prefs = prefs_register_protocol(proto_mdpx, handoff_mdpx);
    prefs_register_uint_preference(prefs, "udp_port", "UDP port (0 disables binding)",
        "MDPX UDP port, 1..65535; Decode As remains available with 0", 10, &udp_port);
    prefs_register_filename_preference(prefs, "trust_file", "Public-key trust file",
        "PUBLIC_KEY_HEX LABEL entries; loaded on preference application or capture open. No private keys.",
        &trust_path, false);
    register_init_routine(load_trust);
}

WS_DLL_PUBLIC_DEF uint32_t plugin_describe(void)
{
    return WS_PLUGIN_DESC_DISSECTOR;
}

WS_DLL_PUBLIC_DEF void plugin_register(void)
{
    static proto_plugin plugin = { register_mdpx, handoff_mdpx };
    proto_register_plugin(&plugin);
}
