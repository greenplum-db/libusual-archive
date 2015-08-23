/* $OpenBSD: tls_verify.c,v 1.7 2015/02/11 06:46:33 jsing Exp $ */
/*
 * Copyright (c) 2014 Jeremie Courreges-Anglas <jca@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "tls_compat.h"

#ifdef USUAL_LIBSSL_FOR_TLS

#include <openssl/x509v3.h>

#include "tls_internal.h"

/*
 * Load cert data from X509 cert.
 */

/* Convert ASN1_INTEGER to decimal string string */
static int tls_parse_bigint(struct tls *ctx, const ASN1_INTEGER *asn1int, const char **dst_p)
{
	long small;
	BIGNUM *big;
	char *tmp, buf[64];

	*dst_p = NULL;
	small = ASN1_INTEGER_get(asn1int);
	if (small < 0) {
		big = ASN1_INTEGER_to_BN(asn1int, NULL);
		if (big) {
			tmp = BN_bn2dec(big);
			if (tmp)
				*dst_p = strdup(tmp);
			OPENSSL_free(tmp);
		}
		BN_free(big);
	} else {
		snprintf(buf, sizeof buf, "%lu", small);
		*dst_p = strdup(buf);
	}
	if (*dst_p)
		return 0;

	tls_set_error(ctx, "cannot parse serial");
	return -1;
}

/* Convert ASN1_TIME to ISO 8601 string */
static int tls_parse_time(struct tls *ctx, const ASN1_TIME *asn1time, const char **dst_p)
{
	static const char months[12][4] = {
		"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};
	char buf[128], *tmp, *mon, *day, *time, *year, *tz;
	char buf2[128];
	BIO *bio;
	int ret, i;

	*dst_p = NULL;

	memset(buf, 0, sizeof buf);
	bio = BIO_new(BIO_s_mem());
	if (!bio)
		goto nomem;

	/* result: Aug 18 20:51:52 2015 GMT */
	ret = ASN1_TIME_print(bio, asn1time);
	if (!ret) {
		BIO_free(bio);
		goto nomem;
	}
	BIO_read(bio, buf, sizeof(buf) - 1);
	BIO_free(bio);
	memcpy(buf2, buf, 128);

	// "Jan  1"
	if (buf[3] == ' ' && buf[4] == ' ')
		buf[4] = '0';

	tmp = buf;
	mon = strsep(&tmp, " ");
	day = strsep(&tmp, " ");
	time = strsep(&tmp, " ");
	year = strsep(&tmp, " ");
	tz = strsep(&tmp, " ");

	if (!year || tmp) {
		tls_set_error(ctx, "invalid time format: no year: %s", buf2);
		return -1;
		goto invalid;
	}
	if (tz && strcmp(tz, "GMT") != 0)
		goto invalid;

	for (i = 0; i < 12; i++) {
		if (memcmp(months[i], mon, 4) == 0)
			break;
	}
	if (i > 11)
		goto invalid;

	ret = asprintf(&tmp, "%s-%02d-%sT%sZ", year, i+1, day, time);
	if (ret < 0)
		goto nomem;
	*dst_p = tmp;
	return 0;

invalid:
	tls_set_error(ctx, "invalid time format");
	return -1;
nomem:
	tls_set_error(ctx, "no mem to parse time");
	return -1;
}

static int
tls_cert_get_name_string(struct tls *ctx, X509_NAME *name, int nid, const char **str_p)
{
	char *res = NULL;
	int res_len;

	*str_p = NULL;

	res_len = X509_NAME_get_text_by_NID(name, nid, NULL, 0);
	if (res_len < 0)
		return 0;

	res = calloc(res_len + 1, 1);
	if (res == NULL) {
		tls_set_error(ctx, "no mem");
		return -1;
	}

	X509_NAME_get_text_by_NID(name, nid, res, res_len + 1);

	/* NUL bytes in value? */
	if (memchr(res, '\0', res_len)) {
		tls_set_error(ctx, "corrupt cert - NUL bytes is value");
		free(res);
		return -2;
	}

	*str_p = res;
	return 0;
}

static int
tls_load_alt_ia5string(struct tls *ctx, ASN1_IA5STRING *ia5str, struct tls_cert_info *cert_info, int slot_type)
{
	struct tls_cert_alt_name *slot;
	char *data;
	int format, len;

	slot = &cert_info->subject_alt_names[cert_info->subject_alt_name_count];

	format = ASN1_STRING_type(ia5str);
	if (format != V_ASN1_IA5STRING) {
		/* ignore unknown string type */
		return 0;
	}

	data = (char *)ASN1_STRING_data(ia5str);
	len = ASN1_STRING_length(ia5str);

	/*
	 * Per RFC 5280 section 4.2.1.6:
	 * disallow empty strings.
	 */
	if (len <= 0 || memchr(data, '\0', len)) {
		tls_set_error(ctx, "invalid string value");
		return -1;
	}

	/*
	 * Per RFC 5280 section 4.2.1.6:
	 * " " is a legal domain name, but that
	 * dNSName must be rejected.
	 */
	if (len == 1 && data[0] == ' ') {
		tls_set_error(ctx, "single space as name");
		return -1;
	}

	slot->alt_name = strdup(data);
	if (slot->alt_name == NULL) {
		tls_set_error(ctx, "no mem");
		return -1;
	}
	slot->alt_name_type = slot_type;

	cert_info->subject_alt_name_count++;
	return 0;
}

static int
tls_load_alt_ipaddr(struct tls *ctx, ASN1_OCTET_STRING *bin, struct tls_cert_info *cert_info)
{
	struct tls_cert_alt_name *slot;
	void *data;
	int len;

	slot = &cert_info->subject_alt_names[cert_info->subject_alt_name_count];
	len = ASN1_STRING_length(bin);
	data = ASN1_STRING_data(bin);
	if (len < 0) {
		tls_set_error(ctx, "negative length for ipaddress");
		return -1;
	}

	/*
	 * Per RFC 5280 section 4.2.1.6:
	 * IPv4 must use 4 octets and IPv6 must use 16 octets.
	 */
	if (len == 4) {
		slot->alt_name_type = TLS_CERT_NAME_IPv4;
	} else if (len == 16) {
		slot->alt_name_type = TLS_CERT_NAME_IPv6;
	} else {
		tls_set_error(ctx, "invalid length for ipaddress");
		return -1;
	}

	slot->alt_name = malloc(len);
	if (slot->alt_name == NULL) {
		tls_set_error(ctx, "no mem");
		return -1;
	}

	memcpy((void *)slot->alt_name, data, len);
	cert_info->subject_alt_name_count++;
	return 0;
}

/* See RFC 5280 section 4.2.1.6 for SubjectAltName details. */
static int
tls_cert_get_altnames(struct tls *ctx, struct tls_cert_info *cert_info, X509 *x509_cert)
{
	STACK_OF(GENERAL_NAME) *altname_stack = NULL;
	GENERAL_NAME *altname;
	int count, i;
	int rv = -1;

	altname_stack = X509_get_ext_d2i(x509_cert, NID_subject_alt_name, NULL, NULL);
	if (altname_stack == NULL)
		return 0;

	count = sk_GENERAL_NAME_num(altname_stack);
	if (count == 0) {
		rv = 0;
		goto out;
	}

	cert_info->subject_alt_names = calloc(sizeof (struct tls_cert_alt_name), count);
	if (cert_info->subject_alt_names == NULL) {
		tls_set_error(ctx, "no mem");
		goto out;
	}

	for (i = 0; i < count; i++) {
		altname = sk_GENERAL_NAME_value(altname_stack, i);

		if (altname->type == GEN_DNS) {
			rv = tls_load_alt_ia5string(ctx, altname->d.dNSName, cert_info, TLS_CERT_NAME_DNS);
		} else if (altname->type == GEN_EMAIL) {
			rv = tls_load_alt_ia5string(ctx, altname->d.rfc822Name, cert_info, TLS_CERT_NAME_EMAIL);
		} else if (altname->type == GEN_URI) {
			rv = tls_load_alt_ia5string(ctx, altname->d.uniformResourceIdentifier, cert_info, TLS_CERT_NAME_URI);
		} else if (altname->type == GEN_IPADD) {
			rv = tls_load_alt_ipaddr(ctx, altname->d.iPAddress, cert_info);
		} else {
			/* ignore unknown types */
		}
		if (rv < 0)
			goto out;
	}
	rv = 0;
out:
	sk_GENERAL_NAME_pop_free(altname_stack, GENERAL_NAME_free);
	return rv;
}

static int
tls_get_entity(struct tls *ctx, X509_NAME *name, struct tls_cert_entity *ent)
{
	int ret;
	ret = tls_cert_get_name_string(ctx, name, NID_commonName, &ent->common_name);
	if (ret == 0)
		ret = tls_cert_get_name_string(ctx, name, NID_countryName, &ent->country_name);
	if (ret == 0)
		ret = tls_cert_get_name_string(ctx, name, NID_stateOrProvinceName, &ent->state_or_province_name);
	if (ret == 0)
		ret = tls_cert_get_name_string(ctx, name, NID_localityName, &ent->locality_name);
	if (ret == 0)
		ret = tls_cert_get_name_string(ctx, name, NID_streetAddress, &ent->street_address);
	if (ret == 0)
		ret = tls_cert_get_name_string(ctx, name, NID_organizationName, &ent->organization_name);
	if (ret == 0)
		ret = tls_cert_get_name_string(ctx, name, NID_organizationalUnitName, &ent->organizational_unit_name);
	return ret;
}

int
tls_get_peer_cert(struct tls *ctx, struct tls_cert_info **cert_p)
{
	struct tls_cert_info *cert = NULL;
	SSL *conn = ctx->ssl_conn;
	X509 *peer;
	X509_NAME *subject, *issuer;
	int ret = -1;
	long version;

	*cert_p = NULL;

	if (!conn) {
		tls_set_error(ctx, "not connected");
		return -1;
	}

	peer = SSL_get_peer_certificate(conn);
	if (!peer) {
		tls_set_error(ctx, "peer does not have cert");
		return -1;
	}

	version = X509_get_version(peer);
	if (version < 0) {
		tls_set_error(ctx, "invalid version");
		return -1;
	}

	subject = X509_get_subject_name(peer);
	if (!subject) {
		tls_set_error(ctx, "cert does not have subject");
		return -1;
	}

	issuer = X509_get_issuer_name(peer);
	if (!issuer) {
		tls_set_error(ctx, "cert does not have issuer");
		return -1;
	}

	cert = calloc(sizeof *cert, 1);
	if (!cert) {
		tls_set_error(ctx, "calloc failed");
		goto failed;
	}
	cert->version = version;

	ret = tls_get_entity(ctx, subject, &cert->subject);
	if (ret == 0)
		ret = tls_get_entity(ctx, issuer, &cert->issuer);
	if (ret == 0)
		ret = tls_cert_get_altnames(ctx, cert, peer);
	if (ret == 0)
		ret = tls_parse_time(ctx, X509_get_notBefore(peer), &cert->not_before);
	if (ret == 0)
		ret = tls_parse_time(ctx, X509_get_notAfter(peer), &cert->not_after);
	if (ret == 0)
		ret = tls_parse_bigint(ctx, X509_get_serialNumber(peer), &cert->serial);
	if (ret == 0) {
		*cert_p = cert;
		return 0;
	}
failed:
	tls_cert_free(cert);
	return ret;
}

static void
tls_cert_free_entity(struct tls_cert_entity *ent)
{
	free(ent->common_name);
	free(ent->country_name);
	free(ent->state_or_province_name);
	free(ent->locality_name);
	free(ent->street_address);
	free(ent->organization_name);
	free(ent->organizational_unit_name);
}

void
tls_cert_free(struct tls_cert_info *cert)
{
	int i;
	if (!cert)
		return;

	tls_cert_free_entity(&cert->issuer);
	tls_cert_free_entity(&cert->subject);

	if (cert->subject_alt_name_count) {
		for (i = 0; i < cert->subject_alt_name_count; i++)
			free(cert->subject_alt_names[i].alt_name);
	}
	free(cert->subject_alt_names);

	free(cert->serial);
	free(cert->not_before);
	free(cert->not_after);
	free(cert);
}

/*
 * Fingerprint calculation.
 */

int
tls_get_peer_cert_fingerprint(struct tls *ctx, const char *algo, void *buf, size_t buflen, size_t *outlen)
{
	SSL *conn = ctx->ssl_conn;
	X509 *peer;
	const EVP_MD *md;
	unsigned char tmpbuf[EVP_MAX_MD_SIZE];
	unsigned int tmplen = 0;
	int ret;

	if (outlen)
		*outlen = 0;

	if (!conn) {
		tls_set_error(ctx, "not connected");
		return -1;
	}

	peer = SSL_get_peer_certificate(conn);
	if (!peer) {
		tls_set_error(ctx, "peer does not have cert");
		return -1;
	}

	if (strcasecmp(algo, "sha1") == 0) {
		md = EVP_sha1();
	} else if (strcasecmp(algo, "sha256") == 0) {
		md = EVP_sha256();
	} else {
		tls_set_error(ctx, "invalid fingerprint algorithm");
		return -1;
	}

	ret = X509_digest(peer, md, tmpbuf, &tmplen);
	if (ret != 1) {
		tls_set_error(ctx, "X509_digest failed");
		return -1;
	}

	if (tmplen > buflen)
		tmplen = buflen;
	memcpy(buf, tmpbuf, tmplen);

	explicit_bzero(tmpbuf, sizeof(tmpbuf));
	if (outlen)
		*outlen = tmplen;

	return 0;
}

#endif /* USUAL_LIBSSL_FOR_TLS */
