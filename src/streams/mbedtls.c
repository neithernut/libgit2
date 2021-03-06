/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "streams/mbedtls.h"

#ifdef GIT_MBEDTLS

#include <ctype.h>

#include "global.h"
#include "stream.h"
#include "streams/socket.h"
#include "netops.h"
#include "git2/transport.h"
#include "util.h"

#ifdef GIT_CURL
# include "streams/curl.h"
#endif

#ifndef GIT_DEFAULT_CERT_LOCATION
#define GIT_DEFAULT_CERT_LOCATION NULL
#endif

/* Work around C90-conformance issues */
#if defined(_MSC_VER)
# define inline __inline
#elif defined(__GNUC__)
# define inline __inline__
#else
# define inline
#endif

#include <mbedtls/config.h>
#include <mbedtls/ssl.h>
#include <mbedtls/error.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>

#undef inline

#define GIT_SSL_DEFAULT_CIPHERS "TLS-ECDHE-ECDSA-WITH-AES-128-GCM-SHA256:TLS-ECDHE-RSA-WITH-AES-128-GCM-SHA256:TLS-ECDHE-ECDSA-WITH-AES-256-GCM-SHA384:TLS-ECDHE-RSA-WITH-AES-256-GCM-SHA384:TLS-DHE-RSA-WITH-AES-128-GCM-SHA256:TLS-DHE-DSS-WITH-AES-128-GCM-SHA256:TLS-DHE-RSA-WITH-AES-256-GCM-SHA384:TLS-DHE-DSS-WITH-AES-256-GCM-SHA384:TLS-ECDHE-ECDSA-WITH-AES-128-CBC-SHA256:TLS-ECDHE-RSA-WITH-AES-128-CBC-SHA256:TLS-ECDHE-ECDSA-WITH-AES-128-CBC-SHA:TLS-ECDHE-RSA-WITH-AES-128-CBC-SHA:TLS-ECDHE-ECDSA-WITH-AES-256-CBC-SHA384:TLS-ECDHE-RSA-WITH-AES-256-CBC-SHA384:TLS-ECDHE-ECDSA-WITH-AES-256-CBC-SHA:TLS-ECDHE-RSA-WITH-AES-256-CBC-SHA:TLS-DHE-RSA-WITH-AES-128-CBC-SHA256:TLS-DHE-RSA-WITH-AES-256-CBC-SHA256:TLS-DHE-RSA-WITH-AES-128-CBC-SHA:TLS-DHE-RSA-WITH-AES-256-CBC-SHA:TLS-DHE-DSS-WITH-AES-128-CBC-SHA256:TLS-DHE-DSS-WITH-AES-256-CBC-SHA256:TLS-DHE-DSS-WITH-AES-128-CBC-SHA:TLS-DHE-DSS-WITH-AES-256-CBC-SHA:TLS-RSA-WITH-AES-128-GCM-SHA256:TLS-RSA-WITH-AES-256-GCM-SHA384:TLS-RSA-WITH-AES-128-CBC-SHA256:TLS-RSA-WITH-AES-256-CBC-SHA256:TLS-RSA-WITH-AES-128-CBC-SHA:TLS-RSA-WITH-AES-256-CBC-SHA"
#define GIT_SSL_DEFAULT_CIPHERS_COUNT 30

mbedtls_ssl_config *git__ssl_conf;
static int ciphers_list[GIT_SSL_DEFAULT_CIPHERS_COUNT];
mbedtls_entropy_context *mbedtls_entropy;

/**
 * This function aims to clean-up the SSL context which
 * we allocated.
 */
static void shutdown_ssl(void)
{
	if (git__ssl_conf) {
		mbedtls_x509_crt_free(git__ssl_conf->ca_chain);
		git__free(git__ssl_conf->ca_chain);
		mbedtls_ctr_drbg_free(git__ssl_conf->p_rng);
		git__free(git__ssl_conf->p_rng);
		mbedtls_ssl_config_free(git__ssl_conf);
		git__free(git__ssl_conf);
		git__ssl_conf = NULL;
	}
	if (mbedtls_entropy) {
		mbedtls_entropy_free(mbedtls_entropy);
		git__free(mbedtls_entropy);
		mbedtls_entropy = NULL;
	}
}

int git_mbedtls__set_cert_location(const char *path, int is_dir);

int git_mbedtls_stream_global_init(void)
{
	int loaded = 0;
	char *crtpath = GIT_DEFAULT_CERT_LOCATION;
	struct stat statbuf;
	mbedtls_ctr_drbg_context *ctr_drbg = NULL;

	size_t ciphers_known = 0;
	char *cipher_name = NULL;
	char *cipher_string = NULL;
	char *cipher_string_tmp = NULL;

	git__ssl_conf = git__malloc(sizeof(mbedtls_ssl_config));
	GITERR_CHECK_ALLOC(git__ssl_conf);

	mbedtls_ssl_config_init(git__ssl_conf);
	if (mbedtls_ssl_config_defaults(git__ssl_conf,
		                            MBEDTLS_SSL_IS_CLIENT,
		                            MBEDTLS_SSL_TRANSPORT_STREAM,
		                            MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
		giterr_set(GITERR_SSL, "failed to initialize mbedTLS");
		goto cleanup;
	}

	/* configure TLSv1 */
	mbedtls_ssl_conf_min_version(git__ssl_conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_0);

	/* verify_server_cert is responsible for making the check.
	 * OPTIONAL because REQUIRED drops the certificate as soon as the check
	 * is made, so we can never see the certificate and override it. */
	mbedtls_ssl_conf_authmode(git__ssl_conf, MBEDTLS_SSL_VERIFY_OPTIONAL);

	/* set the list of allowed ciphersuites */
	ciphers_known = 0;
	cipher_string = cipher_string_tmp = git__strdup(GIT_SSL_DEFAULT_CIPHERS);
	GITERR_CHECK_ALLOC(cipher_string);

	while ((cipher_name = git__strtok(&cipher_string_tmp, ":")) != NULL) {
		int cipherid = mbedtls_ssl_get_ciphersuite_id(cipher_name);
		if (cipherid == 0) continue;

		if (ciphers_known >= ARRAY_SIZE(ciphers_list)) {
			giterr_set(GITERR_SSL, "out of cipher list space");
			goto cleanup;
		}

		ciphers_list[ciphers_known++] = cipherid;
	}
	git__free(cipher_string);

	if (!ciphers_known) {
		giterr_set(GITERR_SSL, "no cipher could be enabled");
		goto cleanup;
	}
	mbedtls_ssl_conf_ciphersuites(git__ssl_conf, ciphers_list);

	/* Seeding the random number generator */
	mbedtls_entropy = git__malloc(sizeof(mbedtls_entropy_context));
	GITERR_CHECK_ALLOC(mbedtls_entropy);

	mbedtls_entropy_init(mbedtls_entropy);

	ctr_drbg = git__malloc(sizeof(mbedtls_ctr_drbg_context));
	GITERR_CHECK_ALLOC(ctr_drbg);

	mbedtls_ctr_drbg_init(ctr_drbg);

	if (mbedtls_ctr_drbg_seed(ctr_drbg,
		                      mbedtls_entropy_func,
		                      mbedtls_entropy, NULL, 0) != 0) {
		giterr_set(GITERR_SSL, "failed to initialize mbedTLS entropy pool");
		goto cleanup;
	}

	mbedtls_ssl_conf_rng(git__ssl_conf, mbedtls_ctr_drbg_random, ctr_drbg);

	/* load default certificates */
	if (crtpath != NULL && stat(crtpath, &statbuf) == 0 && S_ISREG(statbuf.st_mode))
		loaded = (git_mbedtls__set_cert_location(crtpath, 0) == 0);
	if (!loaded && crtpath != NULL && stat(crtpath, &statbuf) == 0 && S_ISDIR(statbuf.st_mode))
		loaded = (git_mbedtls__set_cert_location(crtpath, 1) == 0);

	git__on_shutdown(shutdown_ssl);

	return 0;

cleanup:
	mbedtls_ctr_drbg_free(ctr_drbg);
	git__free(ctr_drbg);
	mbedtls_ssl_config_free(git__ssl_conf);
	git__free(git__ssl_conf);
	git__ssl_conf = NULL;

	return -1;
}

mbedtls_ssl_config *git__ssl_conf;

static int bio_read(void *b, unsigned char *buf, size_t len)
{
	git_stream *io = (git_stream *) b;
	return (int) git_stream_read(io, buf, len);
}

static int bio_write(void *b, const unsigned char *buf, size_t len)
{
	git_stream *io = (git_stream *) b;
	return (int) git_stream_write(io, (const char *)buf, len, 0);
}

static int ssl_set_error(mbedtls_ssl_context *ssl, int error)
{
	char errbuf[512];
	int ret = -1;

	assert(error != MBEDTLS_ERR_SSL_WANT_READ);
	assert(error != MBEDTLS_ERR_SSL_WANT_WRITE);

	if (error != 0)
		mbedtls_strerror( error, errbuf, 512 );

	switch(error) {
		case 0:
		giterr_set(GITERR_SSL, "SSL error: unknown error");
		break;

	case MBEDTLS_ERR_X509_CERT_VERIFY_FAILED:
		giterr_set(GITERR_SSL, "SSL error: %#04x [%x] - %s", error, ssl->session_negotiate->verify_result, errbuf);
		ret = GIT_ECERTIFICATE;
		break;

	default:
		giterr_set(GITERR_SSL, "SSL error: %#04x - %s", error, errbuf);
	}

	return ret;
}

static int ssl_teardown(mbedtls_ssl_context *ssl)
{
	int ret = 0;

	ret = mbedtls_ssl_close_notify(ssl);
	if (ret < 0)
		ret = ssl_set_error(ssl, ret);

	mbedtls_ssl_free(ssl);
	return ret;
}

static int verify_server_cert(mbedtls_ssl_context *ssl)
{
	int ret = -1;

	if ((ret = mbedtls_ssl_get_verify_result(ssl)) != 0) {
		char vrfy_buf[512];
		int len = mbedtls_x509_crt_verify_info(vrfy_buf, sizeof(vrfy_buf), "", ret);
		if (len >= 1) vrfy_buf[len - 1] = '\0'; /* Remove trailing \n */
		giterr_set(GITERR_SSL, "the SSL certificate is invalid: %#04x - %s", ret, vrfy_buf);
		return GIT_ECERTIFICATE;
	}

	return 0;
}

typedef struct {
	git_stream parent;
	git_stream *io;
	bool connected;
	char *host;
	mbedtls_ssl_context *ssl;
	git_cert_x509 cert_info;
} mbedtls_stream;


int mbedtls_connect(git_stream *stream)
{
	int ret;
	mbedtls_stream *st = (mbedtls_stream *) stream;

	if ((ret = git_stream_connect(st->io)) < 0)
		return ret;

	st->connected = true;

	mbedtls_ssl_set_hostname(st->ssl, st->host);

	mbedtls_ssl_set_bio(st->ssl, st->io, bio_write, bio_read, NULL);

	if ((ret = mbedtls_ssl_handshake(st->ssl)) != 0)
		return ssl_set_error(st->ssl, ret);

	return verify_server_cert(st->ssl);
}

int mbedtls_certificate(git_cert **out, git_stream *stream)
{
	unsigned char *encoded_cert;
	mbedtls_stream *st = (mbedtls_stream *) stream;

	const mbedtls_x509_crt *cert = mbedtls_ssl_get_peer_cert(st->ssl);
	if (!cert) {
		giterr_set(GITERR_SSL, "the server did not provide a certificate");
		return -1;
	}

	/* Retrieve the length of the certificate first */
	if (cert->raw.len == 0) {
		giterr_set(GITERR_NET, "failed to retrieve certificate information");
		return -1;
	}

	encoded_cert = git__malloc(cert->raw.len);
	GITERR_CHECK_ALLOC(encoded_cert);
	memcpy(encoded_cert, cert->raw.p, cert->raw.len);

	st->cert_info.parent.cert_type = GIT_CERT_X509;
	st->cert_info.data = encoded_cert;
	st->cert_info.len = cert->raw.len;

	*out = &st->cert_info.parent;

	return 0;
}

static int mbedtls_set_proxy(git_stream *stream, const git_proxy_options *proxy_options)
{
	mbedtls_stream *st = (mbedtls_stream *) stream;

	return git_stream_set_proxy(st->io, proxy_options);
}

ssize_t mbedtls_stream_write(git_stream *stream, const char *data, size_t len, int flags)
{
	size_t read = 0;
	mbedtls_stream *st = (mbedtls_stream *) stream;

	GIT_UNUSED(flags);

	do {
		int error = mbedtls_ssl_write(st->ssl, (const unsigned char *)data + read, len - read);
		if (error <= 0) {
			return ssl_set_error(st->ssl, error);
		}
		read += error;
	} while (read < len);

	return read;
}

ssize_t mbedtls_stream_read(git_stream *stream, void *data, size_t len)
{
	mbedtls_stream *st = (mbedtls_stream *) stream;
	int ret;

	if ((ret = mbedtls_ssl_read(st->ssl, (unsigned char *)data, len)) <= 0)
		ssl_set_error(st->ssl, ret);

	return ret;
}

int mbedtls_stream_close(git_stream *stream)
{
	mbedtls_stream *st = (mbedtls_stream *) stream;
	int ret = 0;

	if (st->connected && (ret = ssl_teardown(st->ssl)) != 0)
		return -1;

	st->connected = false;

	return git_stream_close(st->io);
}

void mbedtls_stream_free(git_stream *stream)
{
	mbedtls_stream *st = (mbedtls_stream *) stream;

	git__free(st->host);
	git__free(st->cert_info.data);
	git_stream_free(st->io);
	mbedtls_ssl_free(st->ssl);
	git__free(st->ssl);
	git__free(st);
}

int git_mbedtls_stream_new(git_stream **out, const char *host, const char *port)
{
	int error;
	mbedtls_stream *st;

	st = git__calloc(1, sizeof(mbedtls_stream));
	GITERR_CHECK_ALLOC(st);

#ifdef GIT_CURL
	error = git_curl_stream_new(&st->io, host, port);
#else
	error = git_socket_stream_new(&st->io, host, port);
#endif

	if (error < 0)
		goto out_err;

	st->ssl = git__malloc(sizeof(mbedtls_ssl_context));
	GITERR_CHECK_ALLOC(st->ssl);
	mbedtls_ssl_init(st->ssl);
	if (mbedtls_ssl_setup(st->ssl, git__ssl_conf)) {
		giterr_set(GITERR_SSL, "failed to create ssl object");
		error = -1;
		goto out_err;
	}

	st->host = git__strdup(host);
	GITERR_CHECK_ALLOC(st->host);

	st->parent.version = GIT_STREAM_VERSION;
	st->parent.encrypted = 1;
	st->parent.proxy_support = git_stream_supports_proxy(st->io);
	st->parent.connect = mbedtls_connect;
	st->parent.certificate = mbedtls_certificate;
	st->parent.set_proxy = mbedtls_set_proxy;
	st->parent.read = mbedtls_stream_read;
	st->parent.write = mbedtls_stream_write;
	st->parent.close = mbedtls_stream_close;
	st->parent.free = mbedtls_stream_free;

	*out = (git_stream *) st;
	return 0;

out_err:
	mbedtls_ssl_free(st->ssl);
	git_stream_free(st->io);
	git__free(st);

	return error;
}

int git_mbedtls__set_cert_location(const char *path, int is_dir)
{
	int ret = 0;
	char errbuf[512];
	mbedtls_x509_crt *cacert;

	assert(path != NULL);

	cacert = git__malloc(sizeof(mbedtls_x509_crt));
	GITERR_CHECK_ALLOC(cacert);

	mbedtls_x509_crt_init(cacert);
	if (is_dir) {
		ret = mbedtls_x509_crt_parse_path(cacert, path);
	} else {
		ret = mbedtls_x509_crt_parse_file(cacert, path);
	}
	/* mbedtls_x509_crt_parse_path returns the number of invalid certs on success */
	if (ret < 0) {
		mbedtls_x509_crt_free(cacert);
		git__free(cacert);
		mbedtls_strerror( ret, errbuf, 512 );
		giterr_set(GITERR_SSL, "failed to load CA certificates: %#04x - %s", ret, errbuf);
		return -1;
	}

	mbedtls_x509_crt_free(git__ssl_conf->ca_chain);
	git__free(git__ssl_conf->ca_chain);
	mbedtls_ssl_conf_ca_chain(git__ssl_conf, cacert, NULL);

	return 0;
}

#else

#include "stream.h"

int git_mbedtls_stream_global_init(void)
{
	return 0;
}

int git_mbedtls_stream_new(git_stream **out, const char *host, const char *port)
{
	GIT_UNUSED(out);
	GIT_UNUSED(host);
	GIT_UNUSED(port);

	giterr_set(GITERR_SSL, "mbedTLS is not supported in this version");
	return -1;
}

int git_mbedtls__set_cert_location(const char *path, int is_dir)
{
	GIT_UNUSED(path);
	GIT_UNUSED(is_dir);

	giterr_set(GITERR_SSL, "mbedTLS is not supported in this version");
	return -1;
}

#endif
