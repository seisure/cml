/*
 * This file is part of GyroidOS
 * Copyright(c) 2013 - 2020 Fraunhofer AISEC
 * Fraunhofer-Gesellschaft zur Förderung der angewandten Forschung e.V.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 (GPL 2), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GPL 2 license for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, see <http://www.gnu.org/licenses/>
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 * Contact Information:
 * Fraunhofer AISEC <gyroidos@aisec.fraunhofer.de>
 */

#include "ssl_util.h"

#include "macro.h"
#include "mem.h"
#include "file.h"

#include <openssl/err.h>
#include <openssl/sha.h>
#include <openssl/pkcs12.h>
#include <openssl/rand.h>
#include <openssl/pem.h>
#include <openssl/conf.h>
#include <openssl/x509v3.h>
#include <openssl/rsa.h>
#include <openssl/engine.h>
#include <openssl/bio.h>
#include <openssl/x509_vfy.h>
#include <openssl/evp.h>

#include <string.h>
#include <sys/stat.h>

//#undef LOGF_LOG_MIN_PRIO
//#define LOGF_LOG_MIN_PRIO LOGF_PRIO_TRACE

/* Properties for device CSR */
#define COUNTRY_C_CSR "DE"
//#define STATE_ST_CSR "Bayern"
//#define CITY_L_CSR "Muenchen"
#define ORGANIZATION_O_CSR "Fraunhofer"
#define ORG_UNIT_OU1_CSR "AISEC"
//#define ORG_UNIT_OU2_CSR "trustme"
#define KEY_USAGE_CSR "critical, digitalSignature,keyEncipherment,nonRepudiation"
#define EXT_KEY_USAGE_CSR "clientAuth"
#define REQ_VERSION_CSR 0L
#define SINGATURE_MD_CSR "SHA256"
/* Cipher for device CSR private key encryption */
#define CIPHER_PW_CSR SN_aes_256_cbc
#define CIPHER_KEY_WRAP SN_aes_256_cbc
/* Cipher for key wrapping with symmetric key
 * This algo determines the necessary length of the key used for key wrapping
 * with symmetric keys. Ensure that the wrapping keys (e.g. generated in usbtoken.c)
 * have a sufficient length when changing this!
 */
#define CIPHER_KEY_WRAP_SKEY SN_id_aes256_wrap
/* RSA key size when keypair is created */
#define RSA_KEY_SIZE_MKKEYP 4096

#define RSA_KEY_EXPONENT RSA_F4
/* Chunk size for reading sig-/hashfiles */
#define SIGN_HASH_BUFFER_SIZE 4096

/*** self provisioning flags and functions */
#define TEST_C "DE"
#define TEST_ST "Bayern"
#define TEST_L "Muenchen"
#define TEST_O "Fraunhofer"
#define TEST_OU1 "AISEC"
#define TEST_OU2 "trustme"
#define TEST_BASIC_CONSTRAINTS "critical,CA:FALSE"
#define TEST_KEY_USAGE_CERT "critical,keyCertSign,cRLSign"
#define TEST_KEY_IDENTIFIER "hash"
#define TEST_CERT_SERIAL 0
#define TEST_NOT_BEFORE 0
#define TEST_NOT_AFTER (60 * 60 * 24 * 365)
#define TEST_CERT_VERSION 2
#define TEST_FRIENDLY_NAME "trust-me test user"

/* creates a certificate containing the public key pkeyp with
 * serial number and validity in days (for user.p12) */
static X509 *
ssl_mkcert(EVP_PKEY *pkeyp, const char *common_name);

/* adds extension nid with name value to a certificate cert */
static int
ssl_add_ext_cert(X509 *cert, int nid, char *value);
/*** self provisioning flags and functions */

/* creates a public key pair */
static EVP_PKEY *
ssl_mkkeypair(rsa_padding_t key_type);

/* creates a CSR of a public key. If tpmkey is set true, openssl-tpm-engine
 * is used to create the request with a TPM-bound key */
static X509_REQ *
ssl_mkreq(EVP_PKEY *pkeyp, const char *common_name, const char *uid, bool tpmkey);

/* adds an extension nid with name value to a stack of extensions*/
static int
add_ext_req(STACK_OF(X509_EXTENSION) * sk, int nid, char *value);

/*configure pkey context for RSA-PSS padding scheme*/
static int
ssl_set_pkey_ctx_rsa_pss(EVP_PKEY_CTX *ctx, const EVP_MD *hash_fct);

ENGINE *tpm_engine = NULL;

#define ssl_print_err()                                                                            \
	int lineno;                                                                                \
	const char *filename;                                                                      \
	unsigned long errnum = ERR_get_error_line(&filename, &lineno);                             \
	ERROR("OpenSSL: %s (file: %s, line %d)", ERR_error_string(errnum, NULL), filename, lineno);

int
ssl_init(bool use_tpm, void *tpm2d_primary_storage_key_pw)
{
	ENGINE_load_builtin_engines(); // load all bundled ENGINEs into memory and make them visible
	if (use_tpm) {
		tpm_engine = ENGINE_by_id("tpm2");
		if (!tpm_engine) {
			ERROR("Could not find TPM2 engine");
			return -1;
		}

		if (!ENGINE_init(tpm_engine)) {
			ERROR("Failed to initialize TPM2 engine");
			goto error;
		}
		if (!ENGINE_set_default_RSA(tpm_engine) || !ENGINE_set_default_RAND(tpm_engine)) {
			ERROR("Failed to set defaults for TPM2 engine");
			goto error;
		}
		// TODO proper auth handling for hierarchies
		// set the SRK passphrase to make storage key usable
		if (tpm2d_primary_storage_key_pw) {
			if (!ENGINE_ctrl_cmd(tpm_engine, "PIN", 0, tpm2d_primary_storage_key_pw,
					     NULL, 0)) {
				ERROR("Failed to set SRK passphrase with TPM2 engine");
				goto error;
			}
		}
	} else {
		ENGINE_register_all_complete(); // register all of them for every algorithm they implement
	}
	//if (!RAND_status())
	//	ERROR("PRNG has not been seeded with enough data");
	return 0;
error:
	ENGINE_free(tpm_engine);
	ENGINE_finish(tpm_engine);
	tpm_engine = NULL;
	return -1;
}

void
ssl_free(void)
{
	if (tpm_engine) {
		ENGINE_free(tpm_engine);
		ENGINE_finish(tpm_engine);
		tpm_engine = NULL;
	}
}

int
ssl_read_pkcs12_token(const char *token_file, const char *passphrase, EVP_PKEY **pkey, X509 **cert,
		      STACK_OF(X509) * *ca)
{
	ASSERT(token_file);
	ASSERT(passphrase);
	ASSERT(pkey);
	ASSERT(cert);
	//ASSERT(ca); // can be NULL if caller is not interested in CA chain
	DEBUG("Reading PKCS#12 file %s", token_file);

	FILE *fp;
	PKCS12 *p12 = NULL;
	char *passphr = mem_strdup(passphrase);
	int ret = -2;

	if (!(fp = fopen(token_file, "rb"))) {
		ERROR("Error opening PKCS#12 file");
		goto end;
	}
	p12 = d2i_PKCS12_fp(fp, NULL);
	fclose(fp);

	if (!p12) {
		ERROR("Error loading PKCS#12 structure");
		goto end;
	}

	// TODO: investigate empty password and passphr == NULL cases
	if (PKCS12_verify_mac(p12, passphrase, -1) != 1) {
		ERROR("Token password wrong");
		ret = -1;
		goto end;
	}

	DEBUG("Token password OK");

	if (!PKCS12_parse(p12, passphr, pkey, cert, ca)) {
		ERROR("Error reading PKCS#12 structure");
		goto end;
	}

	if (*cert) {
		DEBUG("Token contains certificate");
	}
	if (ca && *ca && sk_X509_num(*ca)) {
		DEBUG("Token contains certificate chain");
	}

	ret = 0;
end:
	if (p12)
		PKCS12_free(p12);
	mem_free0(passphr);
	return ret;
}

int
ssl_create_csr(const char *req_file, const char *key_file, const char *passphrase,
	       const char *common_name, const char *uid, bool tpmkey, rsa_padding_t rsa_padding)
{
	ASSERT(req_file);
	ASSERT(key_file);
	ASSERT(common_name);
	ASSERT(uid);

	FILE *fp;
	EVP_PKEY *pkeyp = NULL;
	X509_REQ *req = NULL;
	const EVP_CIPHER *cipher = NULL;
	int pass_len = 0;

	if (!tpmkey) {
		pkeyp = ssl_mkkeypair(rsa_padding);

		if (NULL == pkeyp) {
			ERROR("Error creating public key pair");
			goto error;
		}
	} else {
		// TODO need to figure out a way to provide passphrases defined in tpm2d_shared
		// setting TPM2D_ATT_KEY_PW as cb_data does not work, so right now the engine prompts for the passphrase
		if ((pkeyp = ENGINE_load_private_key(tpm_engine, key_file, NULL, NULL)) == NULL) {
			ERROR("Error loading key pair in TPM");
			goto error;
		}
	}

	if ((req = ssl_mkreq(pkeyp, common_name, uid, tpmkey)) == NULL) {
		ERROR("Error creating CSR");
		goto error;
	}

	DEBUG("CSR created");

	if (!(fp = fopen(req_file, "wb"))) {
		ERROR("Error saving certificate request");
		goto error;
	}

	if (PEM_write_X509_REQ(fp, req) != 1) {
		ERROR("Error writing certificate request");
		fclose(fp);
		goto error;
	}
	fclose(fp);

	if (!tpmkey) {
		if (!(fp = fopen(key_file, "wb"))) {
			ERROR("Error saving CSR private key");
			goto error;
		}

		// consider pkey without password using default values
		if (passphrase) {
			DEBUG("Passphare for device private key imposed");
			pass_len = strlen(passphrase);
			if (!(cipher = EVP_get_cipherbyname(CIPHER_PW_CSR))) {
				ERROR("Error setting up cipher for CSR private key encryption");
				fclose(fp);
				goto error;
			}
		}

		if (!PEM_write_PrivateKey(fp, pkeyp, cipher, (unsigned char *)passphrase, pass_len,
					  NULL, NULL)) {
			ERROR("Error writing CSR private key");
			fclose(fp);
			goto error;
		}
		fclose(fp);
	}

	EVP_PKEY_free(pkeyp);
	X509_REQ_free(req);
	return 0;
error:
	if (pkeyp)
		EVP_PKEY_free(pkeyp);
	if (req)
		X509_REQ_free(req);
	return -1;
}

static X509_REQ *
ssl_mkreq(EVP_PKEY *pkeyp, const char *common_name, const char *uid, UNUSED bool tpmkey)
{
	ASSERT(pkeyp);
	ASSERT(common_name);
	ASSERT(uid);

	X509_REQ *req;
	X509_NAME *name = NULL;
	STACK_OF(X509_EXTENSION) *exts = NULL;

	if ((req = X509_REQ_new()) == NULL) {
		ERROR("Error in creating certificate structure");
		goto error;
	}

	/* Set to Version 1 */
	if (!X509_REQ_set_version(req, REQ_VERSION_CSR)) {
		ERROR("Error setting certificate structure version");
		goto error;
	}

	if (!X509_REQ_set_pubkey(req, pkeyp)) {
		ERROR("Error setting public key to CSR");
		goto error;
	}

	if ((name = X509_REQ_get_subject_name(req)) == NULL) {
		ERROR("Error in getting CSR subject name");
		goto error;
	}

	if (!X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC,
					(const unsigned char *)COUNTRY_C_CSR, -1, -1, 0)) {
		ERROR("Error adding entry to CSR (C)");
		goto error;
	}

	/*
	if (!X509_NAME_add_entry_by_txt(name,"ST",
				MBSTRING_ASC, (const unsigned char *) STATE_ST_CSR, -1, -1, 0)) {
		ERROR("Error adding entry to CSR (ST)");
		goto error;
	}

	if (!X509_NAME_add_entry_by_txt(name,"L",
				MBSTRING_ASC, (const unsigned char *) CITY_L_CSR, -1, -1, 0)) {
		ERROR("Error adding entry to CSR (L)");
		goto error;
	}
	*/

	if (!X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
					(const unsigned char *)ORGANIZATION_O_CSR, -1, -1, 0)) {
		ERROR("Error adding entry to CSR (0)");
		goto error;
	}

	if (!X509_NAME_add_entry_by_txt(name, "OU", MBSTRING_ASC,
					(const unsigned char *)ORG_UNIT_OU1_CSR, -1, -1, 0)) {
		ERROR("Error adding entry to CSR (OU #1)");
		goto error;
	}

	/*
	if (!X509_NAME_add_entry_by_txt(name,"OU",
				MBSTRING_ASC, (const unsigned char *) ORG_UNIT_OU2_CSR, -1, -1, 0)) {
		ERROR("Error adding entry to CSR (OU #2)");
		goto error;
	}
	*/

	if (!X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
					(const unsigned char *)common_name, -1, -1, 0)) {
		ERROR("Error adding entry to CSR (CN)");
		goto error;
	}

	if ((exts = sk_X509_EXTENSION_new_null()) == NULL) {
		ERROR("Error creating CSR extensions");
		goto error;
	}

	/* Standard extenions */
	if (add_ext_req(exts, NID_key_usage, KEY_USAGE_CSR) != 0) {
		ERROR("Error setting CSR extension (NID_key_usage)");
		goto error;
	}

	if (add_ext_req(exts, NID_ext_key_usage, EXT_KEY_USAGE_CSR) != 0) {
		ERROR("Error setting CSR extension (NID_ext_key_usage)");
		goto error;
	}

	// free'd when X509_req is free'd
	char *uri_uuid = mem_printf("URI:UUID:%s", uid);
	if (add_ext_req(exts, NID_subject_alt_name, uri_uuid) != 0) {
		ERROR("Error setting CSR extension (NID_subject_alt_name)");
		goto error;
	}

	/* Now we've created the extensions we add them to the request */
	if (!X509_REQ_add_extensions(req, exts)) {
		ERROR("Error adding extensions to CSR");
		goto error;
	}

	sk_X509_EXTENSION_pop_free(exts, X509_EXTENSION_free);

	DEBUG("Certificate request initialized");

	const EVP_MD *hash_fct;
	if ((hash_fct = EVP_get_digestbyname(SINGATURE_MD_CSR)) == NULL) {
		ERROR("Error in signature verification (unable to initialize hash function)");
		goto error;
	}

	if (!X509_REQ_sign(req, pkeyp, hash_fct)) {
		ERROR("Failed to sign certificate request");
		goto error;
	}

	DEBUG("Certificate request signed");

	return req;

error:
	if (req)
		X509_REQ_free(req);
	return NULL;
}

static EVP_PKEY *
ssl_mkkeypair(rsa_padding_t key_type)
{
	//https://www.openssl.org/docs/man1.1.1/man3/EVP_PKEY_keygen_init.html
	EVP_PKEY_CTX *ctx = NULL;
	EVP_PKEY *pkey = NULL;
	//const EVP_MD *hash_fct = EVP_sha512();

	if (RSA_SSA_PADDING == key_type) {
		ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
	} else if (RSA_PSS_PADDING == key_type) {
		ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA_PSS, NULL);
	} else {
		ERROR("Unsupported key type");
		return NULL;
	}

	if (!ctx) {
		ERROR("Failed to create EVP_PKEY_CTX");
		return NULL;
	}

	if (EVP_PKEY_keygen_init(ctx) <= 0) {
		ERROR("Failed to initialize EVP_PKEY_keygen");
		goto out;
	}

	if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, RSA_KEY_SIZE_MKKEYP) <= 0) {
		ERROR("Failed to set key length");
		goto out;
	}

	/* Generate key */
	if (EVP_PKEY_keygen(ctx, &pkey) <= 0) {
		ERROR("Failed to generate keypair");
		goto out;
	}

out:
	EVP_PKEY_CTX_free(ctx);
	return pkey;
}

static int
add_ext_req(STACK_OF(X509_EXTENSION) * sk, int nid, char *value)
{
	ASSERT(sk);
	X509_EXTENSION *ex;

	if ((ex = X509V3_EXT_conf_nid(NULL, NULL, nid, value)) == NULL)
		return -1;
	if (sk_X509_EXTENSION_push(sk, ex) == 0)
		return -1;

	return 0;
}

int
ssl_wrap_key(EVP_PKEY *pkey, const unsigned char *plain_key, size_t plain_key_len,
	     unsigned char **wrapped_key, int *wrapped_key_len)
{
	ASSERT(pkey);
	ASSERT(plain_key);
	ASSERT(wrapped_key);
	ASSERT(wrapped_key_len);

	int res = -1;
	const EVP_CIPHER *type;
	const size_t max_out_len = plain_key_len + EVP_MAX_BLOCK_LENGTH;

	if (!(type = EVP_get_cipherbyname(CIPHER_KEY_WRAP))) {
		ERROR("Error setting up cipher for key wrapping");
		return res;
	}

	EVP_CIPHER_CTX *ctx;
	if ((ctx = EVP_CIPHER_CTX_new()) == NULL) {
		ERROR("Allocating EVP cipher failed!");
		return res;
	}

	unsigned char *tmpkey = mem_alloc(EVP_PKEY_size(pkey));
	int tmpkeylen = 0;
	int iv_len = EVP_CIPHER_iv_length(type);
	unsigned char *iv_buf = mem_alloc(iv_len);

	unsigned char *out = mem_alloc(max_out_len);

	// TODO: investigate what this barely documented OpenSSL homebrew EVP_Seal* stuff actually does...!
	if (!EVP_SealInit(ctx, type, &tmpkey, &tmpkeylen, iv_buf, &pkey, 1)) {
		WARN("EVP_SealInit failed.");
		goto cleanup;
	}

	int outlen = 0, tmplen = 0;
	if (!EVP_SealUpdate(ctx, out, &tmplen, plain_key, plain_key_len)) {
		WARN("EVP_SealUpdate failed.");
		goto cleanup;
	}
	outlen += tmplen;
	if (!EVP_SealFinal(ctx, out + tmplen, &tmplen)) {
		WARN("EVP_SealFinal failed.");
		goto cleanup;
	}
	outlen += tmplen;

	// TODO: really investigate why we need ek AND iv
	// maybe use protobuf message to serialize
	size_t len = ADD_WITH_OVERFLOW_CHECK((size_t)sizeof(tmpkeylen), tmpkeylen);
	len = ADD_WITH_OVERFLOW_CHECK(len, iv_len);
	len = ADD_WITH_OVERFLOW_CHECK(len, sizeof(outlen));
	len = ADD_WITH_OVERFLOW_CHECK(len, outlen);
	unsigned char *p = mem_alloc(len);
	*wrapped_key_len = len;
	*wrapped_key = p;
	len = sizeof(tmpkeylen);
	memcpy(p, &tmpkeylen, len);
	p += len;
	len = sizeof(outlen);
	memcpy(p, &outlen, len);
	p += len;
	len = iv_len;
	memcpy(p, iv_buf, len);
	p += len;
	len = tmpkeylen;
	memcpy(p, tmpkey, len);
	p += len;
	len = outlen;
	memcpy(p, out, len);
	p += len;

	res = 0;
cleanup:
	mem_memset0(tmpkey, tmpkeylen);
	mem_memset0(iv_buf, iv_len);
	mem_memset0(out, max_out_len);
	mem_free0(tmpkey);
	mem_free0(iv_buf);
	mem_free0(out);
	EVP_CIPHER_CTX_free(ctx);
	return res;
}

int
ssl_unwrap_key(EVP_PKEY *pkey, const unsigned char *wrapped_key, size_t wrapped_key_len,
	       unsigned char **plain_key, int *plain_key_len)
{
	ASSERT(pkey);
	ASSERT(wrapped_key);
	ASSERT(plain_key);
	ASSERT(plain_key_len);

	int res = -1;
	const EVP_CIPHER *type;
	const size_t max_out_len = wrapped_key_len + EVP_MAX_BLOCK_LENGTH;

	if (!(type = EVP_get_cipherbyname(CIPHER_KEY_WRAP))) {
		ERROR("Error setting up cipher for key unwrapping");
		return res;
	}

	int iv_len = EVP_CIPHER_iv_length(type);
	if (wrapped_key_len < 2 * sizeof(int) + iv_len) {
		WARN("Given wrapped key is invalid/corrupted.");
		return res;
	}
	int tmpkeylen = *((int *)wrapped_key);
	wrapped_key += sizeof(int);
	int keylen = *((int *)wrapped_key);
	wrapped_key += sizeof(int);
	if (wrapped_key_len != 2 * sizeof(int) + iv_len + tmpkeylen + keylen) {
		WARN("Given wrapped key is invalid/corrupted.");
		return res;
	}
	const unsigned char *iv_buf = wrapped_key;
	wrapped_key += iv_len;
	const unsigned char *tmpkey = wrapped_key;
	wrapped_key += tmpkeylen;
	const unsigned char *key = wrapped_key;

	EVP_CIPHER_CTX *ctx;
	if ((ctx = EVP_CIPHER_CTX_new()) == NULL) {
		ERROR("Allocating EVP cipher failed!");
		return res;
	}

	unsigned char *out = mem_alloc0(max_out_len);

	// TODO: investigate what this barely documented OpenSSL homebrew EVP_Seal* stuff actually does...!
	if (!EVP_OpenInit(ctx, type, tmpkey, tmpkeylen, iv_buf, pkey)) {
		WARN("EVP_OpenInit failed.");
		goto cleanup;
	}

	int outlen = 0, tmplen = 0;
	if (!EVP_OpenUpdate(ctx, out, &tmplen, key, keylen)) {
		WARN("EVP_OpenUpdate failed.");
		goto cleanup;
	}
	outlen += tmplen;
	if (!EVP_OpenFinal(ctx, out + tmplen, &tmplen)) {
		WARN("EVP_OpenFinal failed.");
		goto cleanup;
	}
	outlen += tmplen;

	*plain_key = out;
	*plain_key_len = outlen;

	res = 0;
cleanup:
	EVP_CIPHER_CTX_free(ctx);
	return res;
}

int
ssl_wrap_key_sym(const unsigned char *kek, const unsigned char *plain_key, size_t plain_key_len,
		 unsigned char **wrapped_key, int *wrapped_key_len)
{
	ASSERT(kek);
	ASSERT(plain_key);
	ASSERT(wrapped_key);
	ASSERT(wrapped_key_len);

	int res = -1;
	int tmplen = 0;
	int outlen = 0;
	const EVP_CIPHER *type;
	const size_t max_out_len = plain_key_len + EVP_MAX_BLOCK_LENGTH;

	if (!(type = EVP_get_cipherbyname(CIPHER_KEY_WRAP_SKEY))) {
		ERROR("Error setting up cipher for key wrapping");
		return res;
	}

	EVP_CIPHER_CTX *ctx;
	if ((ctx = EVP_CIPHER_CTX_new()) == NULL) {
		ERROR("Allocating EVP cipher failed!");
		return res;
	}

	/* see https://mta.openssl.org/pipermail/openssl-users/2018-May/007998.html */
	EVP_CIPHER_CTX_set_flags(ctx, EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);

	/**
	 * static default IV as defined in RFC 3394
	 * TODO: investigate whether a random IV which is passed along with the
	 * 			wrapped key is possible and desirable
	 */
	unsigned char iv[] = { 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6 };

	/* TODO: investigate whether more space may be required */
	unsigned char *out = mem_alloc0(max_out_len);
	if (!out) {
		ERROR("Failed to allocate memory for wrapped key");
		res = -1;
		goto cleanup;
	}

	if (1 != EVP_EncryptInit_ex(ctx, type, NULL, kek, iv)) {
		ERROR("EVP_EncryptInit_ex failed");
		DEBUG("OpenSSL error: %s", ERR_error_string(ERR_get_error(), NULL));
		goto cleanup;
	}

	if (1 != EVP_EncryptUpdate(ctx, out, &tmplen, plain_key, plain_key_len)) {
		ERROR("EVP_EncryptUpdate failed");
		DEBUG("OpenSSL error: %s", ERR_error_string(ERR_get_error(), NULL));
		goto cleanup;
	}
	outlen = tmplen;

	if (1 != EVP_EncryptFinal_ex(ctx, out + tmplen, &tmplen)) {
		ERROR("EVP_EncryptFinal_ex failed");
		DEBUG("OpenSSL error: %s", ERR_error_string(ERR_get_error(), NULL));
		goto cleanup;
	}
	outlen += tmplen;

	*wrapped_key_len = outlen;
	*wrapped_key = out;

	res = 0;
cleanup:
	EVP_CIPHER_CTX_free(ctx);
	return res;
}

int
ssl_unwrap_key_sym(const unsigned char *kek, const unsigned char *wrapped_key,
		   size_t wrapped_key_len, unsigned char **plain_key, int *plain_key_len)
{
	ASSERT(kek);
	ASSERT(plain_key);
	ASSERT(wrapped_key);
	ASSERT(plain_key_len);

	int res = -1;
	int tmplen = 0;
	int outlen = 0;
	const EVP_CIPHER *type;
	const size_t max_out_len = wrapped_key_len + EVP_MAX_BLOCK_LENGTH;

	if (!(type = EVP_get_cipherbyname(CIPHER_KEY_WRAP_SKEY))) {
		ERROR("Error setting up cipher for key wrapping");
		return res;
	}

	EVP_CIPHER_CTX *ctx;
	if ((ctx = EVP_CIPHER_CTX_new()) == NULL) {
		ERROR("Allocating EVP cipher failed!");
		return res;
	}

	/* see https://mta.openssl.org/pipermail/openssl-users/2018-May/007998.html */
	EVP_CIPHER_CTX_set_flags(ctx, EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);

	/**
	 * static default IV as defined in RFC 3394
	 * TODO: investigate whether a random IV which is passed along with the
	 * 			wrapped key is desirable and possible
	 */
	unsigned char iv[] = { 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6 };

	/* TODO: investigate whether more space may be required */
	unsigned char *out = mem_alloc0(max_out_len);
	if (!out) {
		ERROR("Failed to allocate memory for unwrapped key");
		res = -1;
		goto cleanup;
	}

	if (1 != EVP_DecryptInit_ex(ctx, type, NULL, kek, iv)) {
		ERROR("EVP_DecryptInit_ex failed");
		DEBUG("OpenSSL error: %s", ERR_error_string(ERR_get_error(), NULL));
		goto cleanup;
	}

	if (1 != EVP_DecryptUpdate(ctx, out, &tmplen, wrapped_key, wrapped_key_len)) {
		ERROR("EVP_DecryptUpdate failed");
		DEBUG("OpenSSL error: %s", ERR_error_string(ERR_get_error(), NULL));
		goto cleanup;
	}
	outlen = tmplen;

	if (1 != EVP_DecryptFinal_ex(ctx, out + tmplen, &tmplen)) {
		ERROR("EVP_DecryptFinal_ex failed");
		DEBUG("OpenSSL error: %s", ERR_error_string(ERR_get_error(), NULL));
		goto cleanup;
	}
	outlen += tmplen;

	*plain_key_len = outlen;
	*plain_key = out;

	res = 0;
cleanup:
	EVP_CIPHER_CTX_free(ctx);
	return res;
}

int
cb_verify_ignore_time(int ok, X509_STORE_CTX *ctx)
{
	/* Tolerate certificate expiration and not yet valid case */
	if (X509_STORE_CTX_get_error(ctx) == X509_V_ERR_CERT_HAS_EXPIRED ||
	    X509_STORE_CTX_get_error(ctx) == X509_V_ERR_CERT_NOT_YET_VALID) {
		WARN("Ignored certificate not yet valid or expiration error");
		return 1;
	}
	/* Otherwise don't override */
	return ok;
}

int
ssl_verify_certificate(const char *test_cert_file, const char *root_cert_file, bool ignore_time)
{
	X509 *test_cert = NULL;
	X509_STORE *store = NULL;
	X509_STORE_CTX *context = NULL;
	STACK_OF(X509) *chainstack = NULL;
	BIO *stackbio = NULL;
	int ret = 0;

	if ((store = X509_STORE_new()) == NULL) {
		ERROR("Error in certificate verification (setup store)");
		ret = -2;
		goto end;
	}

	if ((context = X509_STORE_CTX_new()) == NULL) {
		ERROR("Error in certificate verification (setup store_ctx)");
		ret = -2;
		goto end;
	}

	if (ignore_time) {
		DEBUG("Certificate expiration and not yet valid case will be ignored");
		X509_STORE_set_verify_cb(store, cb_verify_ignore_time);
	}

	if (!X509_STORE_load_locations(store, root_cert_file, NULL)) {
		ERROR("Failed to load root CA");
		ret = -2;
		goto end;
	}

	stackbio = BIO_new(BIO_s_file());
	if (BIO_read_filename(stackbio, test_cert_file) <= 0) {
		ERROR("Error loading certificate chain");
		ret = -2;
		goto end;
	}

	if (!PEM_read_bio_X509(stackbio, &test_cert, 0, NULL)) {
		ERROR("Failed to load cert from certificate under test");
		ret = -2;
		goto end;
	}

	if (!(chainstack = sk_X509_new_null())) {
		ERROR("Error setting up certificate chain");
		ret = -2;
		goto end;
	}

	X509 *chain_cert = NULL;
	while (PEM_read_bio_X509(stackbio, &chain_cert, 0, NULL)) {
		if (!sk_X509_push(chainstack, chain_cert)) {
			ERROR("Error reading next cert of the chain");
			ret = -2;
			goto end;
		}
		chain_cert = NULL;
	}

	if (!sk_X509_num(chainstack))
		WARN("Certificate under test has no chain");

	if (!X509_STORE_CTX_init(context, store, test_cert, chainstack)) {
		ERROR("Error in certificate verification (init store_ctx)");
		ret = -2;
		goto end;
	}

	int verify_ret = X509_verify_cert(context);
	const char *verify_string =
		X509_verify_cert_error_string(X509_STORE_CTX_get_error(context));

	INFO("Verification return status: %s", verify_string);

	if (verify_ret == 1) {
		DEBUG("Certificate verification successful");
		ret = 0;
	} else {
		if (verify_ret == 0) {
			ret = -1;
			ERROR("Certificate invalid");
		} else {
			ret = -2;
			ERROR("Unexpected failure during certificate validation");
		}

		int store_ctx_error = X509_STORE_CTX_get_error(context);
		int store_ctx_error_depth = X509_STORE_CTX_get_error_depth(context);
		ERROR("Certificate is not valid, error #%d (%s) at cert chain depth: %d",
		      store_ctx_error, verify_string, store_ctx_error_depth);
	}

end:
	if (context != NULL) {
		X509_STORE_CTX_cleanup(context);
		X509_STORE_CTX_free(context);
	}
	if (store != NULL)
		X509_STORE_free(store);
	if (stackbio != NULL)
		BIO_free(stackbio);
	if (chainstack != NULL)
		sk_X509_pop_free(chainstack, X509_free);
	if (test_cert != NULL)
		X509_free(test_cert);
	return ret;
}

static int
ssl_set_pkey_ctx_rsa_pss(EVP_PKEY_CTX *ctx, const EVP_MD *hash_fct)
{
	if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PSS_PADDING) != 1) {
		ssl_print_err();
		ERROR("Error setting RSA PSS padding");
		return -1;
	}

	if (EVP_PKEY_CTX_set_signature_md(ctx, hash_fct) != 1) {
		ssl_print_err();
		ERROR("Error setting signature digest");
		return -1;
	}

	if (EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, hash_fct) != 1) {
		ssl_print_err();
		ERROR("Error setting RSA PSS mgf1 digest");
		return -1;
	}

	// TODO vs RSA_PSS_SALTLEN_AUTO, RSA_PSS_SALTLEN_MAX
	if (EVP_PKEY_CTX_set_rsa_pss_saltlen(ctx, RSA_PSS_SALTLEN_DIGEST) != 1) {
		ssl_print_err();
		ERROR("Error setting RSA PSS saltlen");
		return -1;
	}
	return 0;
}

int
ssl_verify_signature_from_digest(const char *cert_buf, size_t cert_len, const uint8_t *sig_buf,
				 size_t sig_len, const uint8_t *hash, size_t hash_len,
				 const char *digest_algo)
{
	ASSERT(cert_buf);
	ASSERT(sig_buf);
	ASSERT(hash);

	IF_FALSE_RETVAL_ERROR(0 < cert_len, -1);
	IF_FALSE_RETVAL_ERROR(0 < sig_len, -1);
	IF_FALSE_RETVAL_ERROR(0 < hash_len, -1);

	int ret = 0;
	X509 *cert;
	EVP_PKEY *key = NULL;
	BIO *mem;
	EVP_PKEY_CTX *pkey_ctx = NULL;

	// load certificate
	mem = BIO_new(BIO_s_mem());
	BIO_write(mem, cert_buf, cert_len);
	cert = PEM_read_bio_X509(mem, NULL, 0, NULL);
	BIO_free(mem);

	if ((key = X509_get_pubkey(cert)) == NULL) {
		ERROR("Error in signature verification (loading pubkey failed)");
		ret = -2;
		goto error;
	}

	TRACE("Verifying signature...");

	if ((pkey_ctx = EVP_PKEY_CTX_new(key, NULL)) == NULL) {
		ERROR("Allocating EVP_PKEY_CTX failed!");
		ret = -2;
		goto error;
	}

	ret = EVP_PKEY_verify_init(pkey_ctx);
	if (ret != 1) {
		ret = -2;
		ERROR("EVP_PKEY_verify_init failed");
		goto error;
	}

	const EVP_MD *digest_fct;
	if ((digest_fct = EVP_get_digestbyname(digest_algo)) == NULL) {
		ERROR("Error in file hasing (unable to initialize digest hash function");
		ret = -2;
		goto error;
	}

	int key_base_id = EVP_PKEY_base_id(key);

	if (EVP_PKEY_RSA_PSS == key_base_id) {
		DEBUG("Verifying signature with RSA-PSS padding scheme");
		if (0 > (ret = ssl_set_pkey_ctx_rsa_pss(pkey_ctx, digest_fct))) {
			ERROR("Failed to configue ctx for RSA-PSS padding scheme");
			goto error;
		}
	} else if (EVP_PKEY_RSA == key_base_id) {
		DEBUG("Verifying signature with OpenSSL default padding scheme");
		if (EVP_PKEY_CTX_set_signature_md(pkey_ctx, digest_fct) != 1) {
			DEBUG("EVP_PKEY_CTX_set_signature_md failed");
			ret = -2;
			goto error;
		}

	} else {
		ERROR("Unsupported key type");
		ret = -1;
		goto error;
	}

	ret = EVP_PKEY_verify(pkey_ctx, sig_buf, sig_len, hash, hash_len);
	if (ret != 1) {
		ERROR("EVP_PKEY_verify error");
		ssl_print_err();
		// any error
		if (ret == -1) {
			ret = -2;
		}
		// verification failed
		else {
			ret = -1;
		}
	} else {
		DEBUG("Signature successfully verified");
		ret = 0;
	}

error:
	if (cert)
		X509_free(cert);
	if (key)
		EVP_PKEY_free(key);
	if (pkey_ctx)
		EVP_PKEY_CTX_free(pkey_ctx);
	return ret;
}

unsigned char *
ssl_hash_buf(const unsigned char *buf_to_hash, unsigned int buf_len, unsigned int *calc_len,
	     const char *digest_algo)
{
	ASSERT(buf_to_hash);
	ASSERT(digest_algo);

	unsigned char *ret = NULL;
	FILE *fp = NULL;
	const EVP_MD *hash_fct;
	EVP_MD_CTX *md_ctx = NULL;

	if ((hash_fct = EVP_get_digestbyname(digest_algo)) == NULL) {
		ERROR("Error in file hasing (unable to initialize hash function %s)", digest_algo);
		return NULL;
	}

	if ((md_ctx = EVP_MD_CTX_new()) == NULL) {
		ERROR("Allocating EVP_MD failed!");
		fclose(fp);
		return NULL;
	}

	EVP_DigestInit(md_ctx, hash_fct);

	if (!EVP_DigestUpdate(md_ctx, buf_to_hash, buf_len)) {
		ERROR("Error in buffer hashing");
		fclose(fp);
		goto error;
	}

	ret = mem_alloc0(EVP_MAX_MD_SIZE);
	if (EVP_DigestFinal(md_ctx, ret, calc_len) != 1) {
		ERROR("Error in file hashing (computing hash)");
		mem_free0(ret);
		ret = NULL;
		goto error;
	}

error:
	EVP_MD_CTX_free(md_ctx);
	return ret;
}

unsigned char *
ssl_hash_file(const char *file_to_hash, unsigned int *calc_len, const char *hash_algo)
{
	ASSERT(file_to_hash);
	ASSERT(hash_algo);

	unsigned char *ret = NULL;
	FILE *fp = NULL;
	const EVP_MD *hash_fct;
	EVP_MD_CTX *md_ctx = NULL;

	if (!(fp = fopen(file_to_hash, "rb"))) {
		ERROR("Error in file hasing (opening hash file)");
		return NULL;
	}

	if ((hash_fct = EVP_get_digestbyname(hash_algo)) == NULL) {
		ERROR("Error in file hasing (unable to initialize hash function");
		fclose(fp);
		return NULL;
	}

	if ((md_ctx = EVP_MD_CTX_new()) == NULL) {
		ERROR("Allocating EVP_MD failed!");
		fclose(fp);
		return NULL;
	}

	EVP_DigestInit(md_ctx, hash_fct);

	int len = 0;
	unsigned char buffer[SIGN_HASH_BUFFER_SIZE];

	while ((len = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
		if (!EVP_DigestUpdate(md_ctx, buffer, len)) {
			ERROR("Error in file hashing (reading/hashing file failed");
			fclose(fp);
			goto error;
		}
	}
	fclose(fp);

	ret = (unsigned char *)mem_alloc0(EVP_MAX_MD_SIZE);
	if (EVP_DigestFinal(md_ctx, ret, calc_len) != 1) {
		ERROR("Error in file hashing (computing hash)");
		mem_free0(ret);
		ret = NULL;
		goto error;
	}
	/* DEBUG OUTPUT
	char *string = mem_alloc0(*calc_len*2+1);
	for (unsigned int i = 0;  i < *calc_len;  i++) snprintf(string+2*i, 3, "%02x", ret[i]);
	DEBUG("Calc hash: %s", string);
	*/

error:
	EVP_MD_CTX_free(md_ctx);
	return ret;
}

int
ssl_create_pkcs12_token(const char *token_file, const char *cert_file, const char *passphrase,
			const char *user_name, rsa_padding_t rsa_padding)
{
	ASSERT(token_file && passphrase);

	FILE *fp;
	EVP_PKEY *pkey = NULL;
	X509 *cert = NULL;
	PKCS12 *p12 = NULL;
	char *passphr = mem_strdup(passphrase);

	pkey = ssl_mkkeypair(rsa_padding);

	if (NULL == pkey) {
		ERROR("Error creating public-key pair");
		goto error;
	}

	if ((cert = ssl_mkcert(pkey, user_name)) == NULL) {
		ERROR("Error creating certificate");
		goto error;
	}

	DEBUG("Self-Signed certificate created");

	//TODO determine parameters (curr. use defaults). cert could be NULL, depending on what we aim to do.
	if ((p12 = PKCS12_create(passphr, TEST_FRIENDLY_NAME, pkey, cert, NULL, 0, 0, 0, 0, 0)) ==
	    NULL) {
		ERROR("Error creating PKCS#12 softtoken structure");
		goto error;
	}

	DEBUG("Softtoken initialized, setting mac");

	//TODO determine num of iterations
	if (PKCS12_set_mac(p12, passphrase, -1, NULL, 0, PKCS12_DEFAULT_ITER, NULL) != 1) {
		ERROR("Error setting MAC to softtoken");
		goto error;
	}

	DEBUG("Softtoken created");

	if (!(fp = fopen(token_file, "wb"))) {
		ERROR("Error saving PKCS#12 softtoken");
		goto error;
	}
	if (i2d_PKCS12_fp(fp, p12) != 1) {
		ERROR("Error writing PKCS#12 softtoken");
		fclose(fp);
		goto error;
	}
	fclose(fp);

	if (cert_file) {
		if (!(fp = fopen(cert_file, "wb"))) {
			ERROR("Error saving certificate");
			goto error;
		}

		if (PEM_write_X509(fp, cert) != 1) {
			ERROR("Error saving certificate");
			fclose(fp);
			goto error;
		}
		fclose(fp);
		DEBUG("Stored self-signed certificate and softtoken");
	} else {
		DEBUG("Stored softtoken");
	}
	DEBUG("EVP_PKEY_free %p", (void *)pkey);
	EVP_PKEY_free(pkey);
	DEBUG("X509_free %p", (void *)cert);
	X509_free(cert);
	DEBUG("PKCS12_free %p", (void *)p12);
	PKCS12_free(p12);
	DEBUG("mem_free passphr %p", (void *)passphr);
	mem_free0(passphr);
	DEBUG("all free done");
	return 0;
error:
	if (pkey)
		EVP_PKEY_free(pkey);
	if (cert)
		X509_free(cert);
	if (p12)
		PKCS12_free(p12);
	mem_free0(passphr);
	return -1;
}

int
ssl_newpass_pkcs12_token(const char *token_file, const char *oldpass, const char *newpass)
{
	ASSERT(token_file);

	FILE *fp = NULL;
	PKCS12 *p12 = NULL;
	EVP_PKEY *pkey = NULL;
	X509 *cert = NULL;

	if (NULL == oldpass || NULL == newpass) {
		errno = EINVAL;
		goto error;
	}

	if (!(fp = fopen(token_file, "rb"))) {
		ERROR("Error opening PKCS#12 file");
		goto error;
	}
	p12 = d2i_PKCS12_fp(fp, NULL);
	fclose(fp);

	if (!p12) {
		ERROR("Error loading PKCS#12 structure");
		goto error;
	}
	// currently broken in openssl
	/*if (PKCS12_newpass(p12, oldpass, newpass) != 1) {
		ERROR("Changing password!");
		goto error;
	}*/
	if (PKCS12_parse(p12, oldpass, &pkey, &cert, NULL) == 0) {
		ERROR("Error parsing PKCS#12 softtoken");
		goto error;
	}
	// free old token
	PKCS12_free(p12);
	// recreate token with new passphrase
	if ((p12 = PKCS12_create(newpass, TEST_FRIENDLY_NAME, pkey, cert, NULL, 0, 0, 0, 0, 0)) ==
	    NULL) {
		ERROR("Error creating PKCS#12 softtoken structure");
		goto error;
	}
	if (!(fp = fopen(token_file, "wb"))) {
		ERROR("Error saving PKCS#12 softtoken");
		goto error;
	}
	if (i2d_PKCS12_fp(fp, p12) != 1) {
		ERROR("Error writing PKCS#12 softtoken");
		fclose(fp);
		goto error;
	}

	fclose(fp);
	PKCS12_free(p12);
	return 0;
error:
	if (p12)
		PKCS12_free(p12);
	return -1;
}

static X509 *
ssl_mkcert(EVP_PKEY *pkeyp, const char *common_name)
{
	ASSERT(pkeyp);

	X509 *cert;
	X509_NAME *name = NULL;

	if ((cert = X509_new()) == NULL) {
		ERROR("Error creating certificate structure");
		goto error;
	}

	// seems to correspond to v3
	if (!X509_set_version(cert, TEST_CERT_VERSION)) {
		ERROR("Error setting certificate version");
		goto error;
	}

	if (!ASN1_INTEGER_set(X509_get_serialNumber(cert), TEST_CERT_SERIAL)) {
		ERROR("Error setting serial number");
		goto error;
	}

	if (!X509_gmtime_adj(X509_get_notBefore(cert), TEST_NOT_BEFORE)) {
		ERROR("Error setting timestamp to certificate");
		goto error;
	}

	if (!X509_gmtime_adj(X509_get_notAfter(cert), (long)TEST_NOT_AFTER)) {
		ERROR("Error setting timestamp to certificate");
		goto error;
	}

	if (!X509_set_pubkey(cert, pkeyp)) {
		ERROR("Error setting public key to certificate");
		goto error;
	}

	if ((name = X509_get_subject_name(cert)) == NULL) {
		ERROR("Error in getting certificate subject name");
		goto error;
	}

	/* This function creates and adds the entry, working out the
	 * correct string type and performing checks on its length.
	 * Normally we'd check the return value for errors...
	 */
	if (!X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC, (const unsigned char *)TEST_C, -1,
					-1, 0)) {
		ERROR("Error adding entry to certificate");
		goto error;
	}

	if (!X509_NAME_add_entry_by_txt(name, "ST", MBSTRING_ASC, (const unsigned char *)TEST_ST,
					-1, -1, 0)) {
		ERROR("Error adding entry to certificate");
		goto error;
	}

	if (!X509_NAME_add_entry_by_txt(name, "L", MBSTRING_ASC, (const unsigned char *)TEST_L, -1,
					-1, 0)) {
		ERROR("Error adding entry to certificate");
		goto error;
	}

	if (!X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, (const unsigned char *)TEST_O, -1,
					-1, 0)) {
		ERROR("Error adding entry to certificate");
		goto error;
	}

	if (!X509_NAME_add_entry_by_txt(name, "OU", MBSTRING_ASC, (const unsigned char *)TEST_OU1,
					-1, -1, 0)) {
		ERROR("Error adding entry to certificate");
		goto error;
	}

	if (!X509_NAME_add_entry_by_txt(name, "OU", MBSTRING_ASC, (const unsigned char *)TEST_OU2,
					-1, -1, 0)) {
		ERROR("Error adding entry to certificate");
		goto error;
	}

	if (!X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
					(const unsigned char *)common_name, -1, -1, 0)) {
		ERROR("Error adding entry to certificate");
		goto error;
	}

	/* It is self signed so set the issuer name to be the same as the subject. */
	if (!X509_set_issuer_name(cert, name)) {
		ERROR("Error setting certificate issuer name");
		goto error;
	}

	/* Add various extensions: standard extensions */
	if (ssl_add_ext_cert(cert, NID_basic_constraints, TEST_BASIC_CONSTRAINTS) != 0) {
		ERROR("Error adding extensions to certificate");
		goto error;
	}

	if (ssl_add_ext_cert(cert, NID_key_usage, TEST_KEY_USAGE_CERT) != 0) {
		ERROR("Error adding extensions to certificate");
		goto error;
	}

	if (ssl_add_ext_cert(cert, NID_subject_key_identifier, TEST_KEY_IDENTIFIER) != 0) {
		ERROR("Error adding extensions to certifcate");
		goto error;
	}

	DEBUG("Certificate initialized");
	//DEBUG("Error status: %s", ERR_error_string(ERR_get_error(), NULL));

	if (!X509_sign(cert, pkeyp, EVP_sha256())) {
		ERROR("Error signing certificate");
		return NULL;
	}

	DEBUG("Certificate signed");

	return cert;

error:
	return NULL;
}

static int
ssl_add_ext_cert(X509 *cert, int nid, char *value)
{
	ASSERT(cert && value);

	X509_EXTENSION *ex;
	X509V3_CTX ctx;

	/* This sets the 'context' of the extensions. */
	/* No configuration database */
	X509V3_set_ctx_nodb(&ctx);

	/* Issuer and subject certs: both the target since it is self signed,
	 * no request and no CRL
	 * only valid for our self signed test certificates
	 */
	X509V3_set_ctx(&ctx, cert, cert, NULL, NULL, 0);

	if ((ex = X509V3_EXT_conf_nid(NULL, &ctx, nid, value)) == NULL) {
		ERROR("Error creating extension");
		return -1;
	}

	if (!X509_add_ext(cert, ex, -1)) {
		ERROR("Error setting extension");
		return -1;
	}

	X509_EXTENSION_free(ex);
	return 0;
}

int
ssl_self_sign_csr(const char *csr_file, const char *cert_file, const char *key_file, bool tpmkey)
{
	ASSERT(csr_file);
	ASSERT(cert_file);
	ASSERT(key_file);

	int ret = -1;
	BIO *csr = NULL;
	BIO *cert = NULL;
	BIO *key = NULL;
	RSA *key_rsa = NULL;
	EVP_PKEY *key_evp_priv = NULL;
	EVP_PKEY *key_evp_pub = NULL;
	X509_REQ *csr_x509 = NULL;
	X509 *cert_x509 = NULL;
	X509_NAME *csr_name = NULL;
	X509_NAME *cert_name = NULL;
	STACK_OF(X509_EXTENSION) *ext_stack = NULL;

	// read csr
	if ((csr = BIO_new_file(csr_file, "r")) == NULL) {
		ERROR("Error reading CSR file");
		goto error;
	}

	if (!PEM_read_bio_X509_REQ(csr, &csr_x509, NULL, NULL)) {
		ERROR("Error parsing CSR file");
		goto error;
	}

	if (!tpmkey) {
		// read signing key
		if ((key = BIO_new_file(key_file, "r")) == NULL) {
			ERROR("Error reading csr signing priv key");
			goto error;
		}

		if ((key_evp_priv = EVP_PKEY_new()) == NULL) {
			ERROR("Error creating evp priv key");
			goto error;
		}

		if ((PEM_read_bio_RSAPrivateKey(key, &key_rsa, NULL, NULL)) == NULL) {
			ERROR("error reading rsa priv key");
			goto error;
		}

		if (EVP_PKEY_assign_RSA(key_evp_priv, key_rsa) != 1) {
			ERROR("error assigning rsa priv key");
			RSA_free(key_rsa);
			goto error;
		}
	} else {
		DEBUG("Load key for signing into TPM");
		// TODO same as above, need proper passphrase handling to avoid input prompt
		if ((key_evp_priv = ENGINE_load_private_key(tpm_engine, key_file, NULL, NULL)) ==
		    NULL) {
			ERROR("Error loading csr signing key pair into TPM");
			goto error;
		}
	}

	// create certificate structure
	if ((cert_x509 = X509_new()) == NULL) {
		ERROR("Error creating certificate structure");
		goto error;
	}

	if (!X509_set_version(cert_x509, X509_REQ_get_version(csr_x509))) {
		ERROR("Error setting certificate version");
		goto error;
	}

	if (!ASN1_INTEGER_set(X509_get_serialNumber(cert_x509), TEST_CERT_SERIAL)) {
		ERROR("Error setting serial number");
		goto error;
	}

	if (!X509_gmtime_adj(X509_get_notBefore(cert_x509), TEST_NOT_BEFORE)) {
		ERROR("Error setting timestamp to certificate");
		goto error;
	}

	if (!X509_gmtime_adj(X509_get_notAfter(cert_x509), (long)TEST_NOT_AFTER)) {
		ERROR("Error setting timestamp to certificate");
		goto error;
	}

	if ((key_evp_pub = X509_REQ_get_pubkey(csr_x509)) == NULL) {
		ERROR("Error reading public key from CSR");
		goto error;
	}

	if (!X509_set_pubkey(cert_x509, key_evp_pub)) {
		ERROR("Error setting public key to certificate");
		goto error;
	}

	if ((csr_name = X509_REQ_get_subject_name(csr_x509)) == NULL) {
		ERROR("Error in getting certificate subject name from CSR");
		goto error;
	}

	if ((cert_name = X509_NAME_dup(csr_name)) == NULL) {
		ERROR("Error reading subject name from CSR");
		goto error;
	}

	if (!X509_set_subject_name(cert_x509, cert_name)) {
		ERROR("Error setting certificate subject");
		goto error;
	}

	/* It is self signed so set the issuer name to be the same as the subject. */
	if (!X509_set_issuer_name(cert_x509, csr_name)) {
		ERROR("Error setting certificate issuer name");
		goto error;
	}

	X509_EXTENSION *ext;
	ext_stack = X509_REQ_get_extensions(csr_x509);
	int i;
	for (i = 0; i < sk_X509_EXTENSION_num(ext_stack); i++) {
		ext = sk_X509_EXTENSION_value(ext_stack, i);
		if (!X509_add_ext(cert_x509, ext, -1)) {
			ERROR("Error copying extensions from req to cert");
			goto error;
		}
	}

	DEBUG("Self-sign device cert initialized");

	// sign cert
	if (!X509_sign(cert_x509, key_evp_priv, EVP_sha256())) {
		ERROR("Error signing certificate");
		goto error;
	}

	// write final certificate
	if ((cert = BIO_new_file(cert_file, "w")) == NULL) {
		ERROR("Error opening output cert file for writing");
		goto error;
	}

	if (!PEM_write_bio_X509(cert, cert_x509)) {
		ERROR("Error writing cert file");
		goto error;
	}

	DEBUG("Successfully created self-signed device cert");

	ret = 0;

error:
	if (csr)
		BIO_free(csr);
	if (cert)
		BIO_free(cert);
	if (key)
		BIO_free(key);
	if (key_evp_priv)
		EVP_PKEY_free(key_evp_priv);
	if (cert_x509)
		X509_free(cert_x509);
	if (csr_x509)
		X509_REQ_free(csr_x509);
	// EVP_PKEY_free also frees rsa key if assigned
	if (key_evp_pub)
		EVP_PKEY_free(key_evp_pub);
	if (ext_stack)
		sk_X509_EXTENSION_pop_free(ext_stack, X509_EXTENSION_free);
	return ret;
}

const char *
get_digest_name_by_sig_algo_obj(const ASN1_OBJECT *obj)
{
	// TODO which algorithms shall be supported
	switch (OBJ_obj2nid(obj)) {
	case NID_rsassaPss:
		return "sha256";
	case NID_sha256WithRSAEncryption:
		return "sha256";
	case NID_sha384WithRSAEncryption:
		return "sha384";
	case NID_sha512WithRSAEncryption:
		return "sha512";
	case NID_sha224WithRSAEncryption:
		return "sha224";
	default:
		return NULL;
	}
	return NULL;
}

int
ssl_verify_signature_from_buf(uint8_t *cert_buf, size_t cert_len, const uint8_t *sig_buf,
			      size_t sig_len, const uint8_t *buf, size_t buf_len,
			      const char *digest_algo)

{
	ASSERT(cert_buf);
	ASSERT(sig_buf);
	ASSERT(buf);

	int ret = 0;

	// certificate variables
	X509 *cert;
	BIO *mem;

	// load certificate
	mem = BIO_new(BIO_s_mem());
	BIO_write(mem, cert_buf, cert_len);
	cert = PEM_read_bio_X509(mem, NULL, 0, NULL);
	BIO_free(mem);

	DEBUG("Hash algo: %s", digest_algo);
	unsigned int hash_len = 0;
	unsigned char *hash = ssl_hash_buf(buf, buf_len, &hash_len, digest_algo);

	if (0 > (ret = ssl_verify_signature_from_digest((char *)cert_buf, cert_len, sig_buf,
							sig_len, hash, hash_len, digest_algo))) {
		ssl_print_err();
		ERROR("Failed to verify signature");
		ret = -2;
	}

	mem_free0(hash);
	if (cert)
		X509_free(cert);

	return ret;
}

int
ssl_verify_signature(const char *cert_file, const char *signature_file, const char *signed_file,
		     const char *digest_algo)
{
	ASSERT(cert_file);
	ASSERT(signature_file);
	ASSERT(signed_file);

	off_t cert_len = file_size(cert_file);
	char *cert_buf = mem_alloc0(cert_len);

	if (0 > file_read(cert_file, cert_buf, cert_len)) {
		ERROR("Failed to read cert file");
		return -1;
	}

	int sig_len = file_size(signature_file);
	unsigned char *sig_buf = mem_alloc0(sig_len);

	if (0 > file_read(signature_file, (char *)sig_buf, sig_len)) {
		ERROR("Failed to read signature file");
		return -1;
	}

	unsigned int hash_len;
	unsigned char *hash = ssl_hash_file(signed_file, &hash_len, digest_algo);

	if (!hash) {
		ERROR("Failed to hash file: %s", signed_file);
		return -1;
	}

	int ret = ssl_verify_signature_from_digest(cert_buf, cert_len, sig_buf, sig_len, hash,
						   hash_len, digest_algo);

	return ret;
}

int
ssl_aes_ecb_encrypt(uint8_t *in, int inlen, uint8_t *out, int *outlen, uint8_t *key, int keylen,
		    int pad)
{
	ASSERT(in);
	ASSERT(out);
	ASSERT(key);

	int finallen = 0;
	int ret = 0;
	int out_ret = -1;
	const EVP_CIPHER *cipher;
	if (keylen == 16) {
		cipher = EVP_aes_128_ecb();
	} else if (keylen == 32) {
		cipher = EVP_aes_256_ecb();
	} else {
		ERROR("Unsupported key length %d (only 128-bit and 256-bit supported)", keylen);
		return -1;
	}

	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (!ctx) {
		ERROR("Failed to allocate context for AES-ECB");
		return -1;
	}

	ret = EVP_CIPHER_CTX_init(ctx);
	if (!ret) {
		ERROR("Failed to init context: %d", ret);
		goto out;
	}

	ret = EVP_EncryptInit_ex(ctx, cipher, NULL, key, NULL);
	if (!ret) {
		ERROR("Failed to init context for encryption: %d", ret);
		goto out;
	}

	ret = EVP_CIPHER_CTX_set_padding(ctx, pad);
	if (!ret) {
		ERROR("Failed to set padding: %d", ret);
		goto out;
	}

	// The maximum ciphertext length for n bytes of plaintext is n + AES_BLOCK_SIZE -1 bytes
	int block_size = EVP_CIPHER_block_size(cipher);
	int pad_len = (inlen % block_size) ? (block_size - (inlen % block_size)) : 0;
	int maxlen = ADD_WITH_OVERFLOW_CHECK(inlen, pad_len);
	if (*outlen < maxlen) {
		ERROR("Output buffer too small (%d, must be at least %d)", *outlen, maxlen);
		goto out;
	}

	ret = EVP_EncryptUpdate(ctx, out, outlen, in, inlen);
	if (!ret) {
		ERROR("Failed to update encryption: %d", ret);
		goto out;
	}

	ret = EVP_EncryptFinal_ex(ctx, out + *outlen, &finallen);
	if (!ret) {
		ERROR("Failed to finalize encryption: %d", ret);
		goto out;
	}

	*outlen += finallen;
	out_ret = 0;

out:
	EVP_CIPHER_CTX_free(ctx);
	return out_ret;
}

int
ssl_aes_ecb_decrypt(uint8_t *in, int inlen, uint8_t *out, int *outlen, uint8_t *key, int keylen,
		    int pad)
{
	ASSERT(in);
	ASSERT(out);
	ASSERT(key);

	int ret = 0;
	int out_ret = -1;
	int finallen = 0;
	const EVP_CIPHER *cipher;
	if (keylen == 16) {
		cipher = EVP_aes_128_ecb();
	} else if (keylen == 32) {
		cipher = EVP_aes_256_ecb();
	} else {
		ERROR("Unsupported key length %d", keylen);
		return -1;
	}

	if (*outlen < inlen) {
		ERROR("Output buffer too small (%d, must be at least %d)", *outlen, inlen);
		return -1;
	}

	TRACE("Decrypting buffer with size %d", inlen);

	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (!ctx) {
		ERROR("Failed to allocate context for AES-ECB");
		return -1;
	}

	ret = EVP_CIPHER_CTX_init(ctx);
	if (!ret) {
		ERROR("Failed to init context: %d", ret);
		goto out;
	}

	ret = EVP_DecryptInit_ex(ctx, cipher, NULL, key, NULL);
	if (!ret) {
		ERROR("Failed init context for decryption: %d", ret);
		goto out;
	}

	ret = EVP_CIPHER_CTX_set_padding(ctx, pad);
	if (!ret) {
		ERROR("Failed to set padding: %d", ret);
		goto out;
	}

	ret = EVP_DecryptUpdate(ctx, out, outlen, in, inlen);
	if (!ret) {
		ERROR("Failed to decrypt update: %d", ret);
		goto out;
	}

	ret = EVP_DecryptFinal_ex(ctx, out + *outlen, &finallen);
	if (!ret) {
		ERROR("Failed to decrypt final: %d", ret);
		goto out;
	}

	*outlen += finallen;
	out_ret = 0;

	TRACE("Decrypted buffer, plaintext length: %d", *outlen);

out:
	EVP_CIPHER_CTX_free(ctx);
	return out_ret;
}

EVP_CIPHER_CTX *
ssl_aes_ctr_init_encrypt(uint8_t *key, int keylen, uint8_t *iv, int ivlen)
{
	ASSERT(key);
	ASSERT(iv);

	int ret = 0;
	const EVP_CIPHER *cipher;
	if (keylen == 16) {
		cipher = EVP_aes_128_ctr();
	} else if (keylen == 32) {
		cipher = EVP_aes_256_ctr();
	} else {
		ERROR("Unsupported key length %d (only 128-bit and 256-bit supported)", keylen);
		return NULL;
	}

	if (ivlen != EVP_CIPHER_iv_length(cipher)) {
		ERROR("Invalid iv length %d", ivlen);
		return NULL;
	}

	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (!ctx) {
		ERROR("Failed to allocate context for AES-CTR");
		return NULL;
	}

	ret = EVP_CIPHER_CTX_init(ctx);
	if (!ret) {
		ERROR("Failed to init context: %d", ret);
		goto out;
	}

	ret = EVP_EncryptInit_ex(ctx, cipher, NULL, key, iv);
	if (!ret) {
		ERROR("Failed to init context for AES-CTR encryption: %d", ret);
		goto out;
	}

	return ctx;

out:
	EVP_CIPHER_CTX_free(ctx);
	return NULL;
}

EVP_CIPHER_CTX *
ssl_aes_ctr_init_decrypt(uint8_t *key, int keylen, uint8_t *iv, int ivlen)
{
	ASSERT(key);
	ASSERT(iv);

	int ret = 0;
	const EVP_CIPHER *cipher;
	if (keylen == 16) {
		cipher = EVP_aes_128_ctr();
	} else if (keylen == 32) {
		cipher = EVP_aes_256_ctr();
	} else {
		ERROR("Unsupported key length %d (only 128-bit and 256-bit supported)", keylen);
		return NULL;
	}

	if (ivlen != EVP_CIPHER_iv_length(cipher)) {
		ERROR("Invalid iv length %d", ivlen);
		return NULL;
	}

	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (!ctx) {
		ERROR("Failed to allocate context for AES-CTR");
		return NULL;
	}

	ret = EVP_CIPHER_CTX_init(ctx);
	if (!ret) {
		ERROR("Failed to init context: %d", ret);
		goto out;
	}

	ret = EVP_DecryptInit_ex(ctx, cipher, NULL, key, iv);
	if (!ret) {
		ERROR("Failed to init context for AES-CTR encryption: %d", ret);
		goto out;
	}

	return ctx;

out:
	EVP_CIPHER_CTX_free(ctx);
	return NULL;
}

int
ssl_aes_ctr_encrypt(EVP_CIPHER_CTX *ctx, uint8_t *in, int inlen, uint8_t *out, int *outlen)
{
	int finallen = 0;

	int ret = EVP_EncryptUpdate(ctx, out, outlen, in, inlen);
	if (!ret) {
		ERROR("Failed to update encryption: %d", ret);
		return -1;
	}

	ret = EVP_EncryptFinal_ex(ctx, out + *outlen, &finallen);
	if (!ret) {
		ERROR("Failed to finalize encryption: %d", ret);
		return -1;
	}

	return 0;
}

int
ssl_aes_ctr_decrypt(EVP_CIPHER_CTX *ctx, uint8_t *in, int inlen, uint8_t *out, int *outlen)
{
	int finallen = 0;
	int ret = EVP_DecryptUpdate(ctx, out, outlen, in, inlen);
	if (!ret) {
		ERROR("Failed to decrypt update: %d", ret);
		return -1;
	}

	ret = EVP_DecryptFinal_ex(ctx, out + *outlen, &finallen);
	if (!ret) {
		ERROR("Failed to decrypt final: %d", ret);
		return -1;
	}

	return 0;
}

void
ssl_aes_ctr_free(EVP_CIPHER_CTX *ctx)
{
	EVP_CIPHER_CTX_free(ctx);
}
