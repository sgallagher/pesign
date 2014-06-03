/*
 * Copyright 2014 Red Hat, Inc.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Author(s): Peter Jones <pjones@redhat.com>
 */

#include <err.h>
#include <sys/mman.h>

#include <prerror.h>

#include "pesign.h"

static int
pe_is_valid(void *addr, size_t len)
{
	if (len < 2)
		return 0;
	if (!memcmp(addr, "MZ", 2))
		return 1;
	return 0;
}

static void
pe_setup(pesign_context *ctx, void *addr, size_t len)
{
	ctx->inpe = pe_memory(ctx->inmap, ctx->insize);
	if (!ctx->inpe)
		peerr(1, "Could not load input file");

	int rc = parse_pe_signatures(&ctx->cms_ctx->signatures,
				  &ctx->cms_ctx->num_signatures, ctx->inpe);
	if (rc < 0)
		errx(1, "could not parse signature list in EFI binary");
}

static void
pe_teardown(pesign_context *ctx)
{
	pe_end(ctx->inpe);
	ctx->inpe = NULL;

	munmap(ctx->inmap, ctx->insize);
	ctx->inmap = MAP_FAILED;
	ctx->insize = -1;
}

static int saw_content;

static void
handle_bytes(void *arg, const char *buf, unsigned long len)
{
	saw_content = 1;
}

static PRBool
decryption_allowed(SECAlgorithmID *algid, PK11SymKey *key)
{
	return PR_TRUE;
}


int
list_pe_signatures(pesign_context *ctx)
{
	pe_cert_iter iter;

	int rc = pe_cert_iter_init(&iter, ctx->inpe);

	if (rc < 0) {
		printf("No certificate list found.\n");
		return rc;
	}

	void *data;
	ssize_t datalen;
	int nsigs = 0;

	rc = 0;
	while (1) {
		rc = next_pe_cert(&iter, &data, &datalen);
		if (rc <= 0)
			break;

		SEC_PKCS7DecoderContext *dc = NULL;
		saw_content = 0;
		dc = SEC_PKCS7DecoderStart(handle_bytes, NULL, NULL, NULL,
					NULL, NULL, decryption_allowed);

		if (dc == NULL)
			nsserr(1, "SEC_PKCS7DecoderStart failed");

		SECStatus status = SEC_PKCS7DecoderUpdate(dc, data, datalen);
		if (status != SECSuccess) {
			fprintf(stderr, "Found invalid certificate\n");
			continue;
		}

		SEC_PKCS7ContentInfo *cinfo = SEC_PKCS7DecoderFinish(dc);

		if (cinfo == NULL) {
			fprintf(stderr, "Found invalid certificate\n");
			continue;
		}

		nsigs++;
		printf("---------------------------------------------\n");
		printf("certificate address is %p\n", data);
		printf("Content was%s encrypted.\n",
			SEC_PKCS7ContentIsEncrypted(cinfo) ? "" : " not");
		if (SEC_PKCS7ContentIsSigned(cinfo)) {
			char *signer_cname, *signer_ename;
			SECItem *signing_time;

			if (saw_content) {
				printf("Signature is ");
				PORT_SetError(0);
				if (SEC_PKCS7VerifySignature(cinfo,
						certUsageEmailSigner,
						PR_FALSE)) {
					printf("valid.\n");
				} else {
					printf("invalid (Reason: 0x%08x).\n",
						(uint32_t)PORT_GetError());
				}
			} else {
				printf("Content is detached; signature cannot "
					"be verified.\n");
			}

			signer_cname = SEC_PKCS7GetSignerCommonName(cinfo);
			if (signer_cname != NULL) {
				printf("The signer's common name is %s\n",
					signer_cname);
				PORT_Free(signer_cname);
			} else {
				printf("No signer common name.\n");
			}

			signer_ename = SEC_PKCS7GetSignerEmailAddress(cinfo);
			if (signer_ename != NULL) {
				printf("The signer's email address is %s\n",
					signer_ename);
				PORT_Free(signer_ename);
			} else {
				printf("No signer email address.\n");
			}

			signing_time = SEC_PKCS7GetSigningTime(cinfo);
			if (signing_time != NULL) {
				printf("Signing time: %s\n", DER_TimeChoiceDayToAscii(signing_time));
			} else {
				printf("No signing time included.\n");
			}

			printf("There were%s certs or crls included.\n",
				SEC_PKCS7ContainsCertsOrCrls(cinfo) ? "" : " no");

			SEC_PKCS7DestroyContentInfo(cinfo);
		}
	}
	if (nsigs) {
		printf("---------------------------------------------\n");
	} else {
		printf("No signatures found.\n");
	}
	return rc;
}

void
assert_pe_signature_space(pesign_context *ctx)
{
	ssize_t available = available_pe_cert_space(ctx->outpe);

	if (available < ctx->cms_ctx->newsig.len)
		errx(1, "Could not add new signature: insufficient space");
}

void
allocate_pe_signature_space(pesign_context *ctx, ssize_t sigspace)
{
	int rc;

	rc = pe_alloccert(ctx->outpe, sigspace);
	if (rc < 0)
		err(1, "Could not allocate space for signature");
}

const file_handlers_t pe_handlers = {
	.is_valid = pe_is_valid,
	.setup = pe_setup,
	.teardown = pe_teardown,
	.list_signatures = list_pe_signatures,
	.allocate_signature_space = allocate_pe_signature_space,
	.assert_signature_space = assert_pe_signature_space,
};
