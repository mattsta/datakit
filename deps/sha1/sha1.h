/*	$NetBSD: sha1.h,v 1.13 2005/12/26 18:41:36 perry Exp $	*/

/*
 * SHA-1 in C
 * By Steve Reid <steve@edmweb.com>
 * 100% Public Domain
 */

#ifndef SHA1_H
#define SHA1_H

#include <stdint.h>
#include <sys/cdefs.h>
#include <sys/types.h>

#define SHA1_DIGEST_LENGTH 20
#define SHA1_DIGEST_STRING_LENGTH 41

typedef struct {
    uint32_t state[5];
    uint32_t count[2];
    uint8_t buffer[64];
} SHA1_CTX;

__BEGIN_DECLS
void SHA1Transform(uint32_t[5], const uint8_t[64]);
void SHA1Init(SHA1_CTX *);
void SHA1Update(SHA1_CTX *, const uint8_t *, uint32_t);
void SHA1Final(uint8_t[SHA1_DIGEST_LENGTH], SHA1_CTX *);

#ifdef DATAKIT_TEST
int sha1Test(int argc, char **argv);
#endif
__END_DECLS

#endif /* SHA1_H */
