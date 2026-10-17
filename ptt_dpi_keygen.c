/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dpi/mdpx.h"
#include <sodium.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    const char *secret = NULL, *public = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--secret") && i + 1 < argc) secret = argv[++i];
        else if (!strcmp(argv[i], "--public") && i + 1 < argc) public = argv[++i];
        else { fprintf(stderr, "usage: %s --secret FILE --public FILE\n", argv[0]); return 1; }
    }
    if (!secret || !public || sodium_init() < 0) {
        fprintf(stderr, "Require --secret FILE and --public FILE, and libsodium initialization\n");
        return 1;
    }
    if (keygen(secret, public)) {
        fprintf(stderr, "Key generation failed; files must not already exist. A newly created secret may remain if public export failed.\n");
        return 1;
    }
    printf("Created signing seed %s and observer public key %s\n", secret, public);
    return 0;
}
