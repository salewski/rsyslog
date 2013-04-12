/* This is a tool for processing rsyslog encrypted log files.
 * 
 * Copyright 2013 Adiscon GmbH
 *
 * This file is part of rsyslog.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *       http://www.apache.org/licenses/LICENSE-2.0
 *       -or-
 *       see COPYING.ASL20 in the source distribution
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either exprs or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <getopt.h>
#include <gcrypt.h>

#include "rsyslog.h"
#include "libgcry.h"


static enum { MD_DECRYPT
} mode = MD_DECRYPT;
static int verbose = 0;
static gcry_cipher_hd_t gcry_chd;
static size_t blkLength;


/* rectype/value must be EIF_MAX_*_LEN+1 long!
 * returns 0 on success or something else on error/EOF
 */
static int
eiGetRecord(FILE *eifp, char *rectype, char *value)
{
	int r;
	unsigned short i, j;
	char buf[EIF_MAX_RECTYPE_LEN+EIF_MAX_VALUE_LEN+128];
	     /* large enough for any valid record */

	if(fgets(buf, sizeof(buf), eifp) == NULL) {
		r = 1; goto done;
	}

	for(i = 0 ; i < EIF_MAX_RECTYPE_LEN && buf[i] != ':' ; ++i)
		if(buf[i] == '\0') {
			r = 2; goto done;
		} else 
			rectype[i] = buf[i];
	rectype[i] = '\0';
	j = 0;
	for(++i ; i < EIF_MAX_VALUE_LEN && buf[i] != '\n' ; ++i, ++j)
		if(buf[i] == '\0') {
			r = 3; goto done;
		} else 
			value[j] = buf[i];
	value[j] = '\0';
	r = 0;
done:	return r;
}

static int
eiCheckFiletype(FILE *eifp)
{
	char rectype[EIF_MAX_RECTYPE_LEN+1];
	char value[EIF_MAX_VALUE_LEN+1];
	int r;

	if((r = eiGetRecord(eifp, rectype, value)) != 0) goto done;
	if(strcmp(rectype, "FILETYPE") || strcmp(value, RSGCRY_FILETYPE_NAME)) {
		fprintf(stderr, "invalid filetype \"cookie\" in encryption "
			"info file\n");
		fprintf(stderr, "\trectype: '%s', value: '%s'\n", rectype, value);
		r = 1; goto done;
	}
	r = 0;
done:	return r;
}

static int
eiGetIV(FILE *eifp, char *iv, size_t leniv)
{
	char rectype[EIF_MAX_RECTYPE_LEN+1];
	char value[EIF_MAX_VALUE_LEN+1];
	size_t valueLen;
	unsigned short i, j;
	int r;
	unsigned char nibble;

	if((r = eiGetRecord(eifp, rectype, value)) != 0) goto done;
	if(strcmp(rectype, "IV")) {
		fprintf(stderr, "no IV record found when expected, record type "
			"seen is '%s'\n", rectype);
		r = 1; goto done;
	}
	valueLen = strlen(value);
	if(valueLen/2 != leniv) {
		fprintf(stderr, "length of IV is %d, expected %d\n",
			valueLen/2, leniv);
		r = 1; goto done;
	}

	for(i = j = 0 ; i < valueLen ; ++i) {
		if(value[i] >= '0' && value[i] <= '9')
			nibble = value[i] - '0';
		else if(value[i] >= 'a' && value[i] <= 'f')
			nibble = value[i] - 'a' + 10;
		else {
			fprintf(stderr, "invalid IV '%s'\n", value);
			r = 1; goto done;
		}
		if(i % 2 == 0)
			iv[j] = nibble << 4;
		else
			iv[j++] |= nibble;
	}
	r = 0;
done:	return r;
}

static int
eiGetEND(FILE *eifp, off64_t *offs)
{
	char rectype[EIF_MAX_RECTYPE_LEN+1];
	char value[EIF_MAX_VALUE_LEN+1];
	int r;

	if((r = eiGetRecord(eifp, rectype, value)) != 0) goto done;
	if(strcmp(rectype, "END")) {
		fprintf(stderr, "no END record found when expected, record type "
			"seen is '%s'\n", rectype);
		r = 1; goto done;
	}
	*offs = atoll(value);
	r = 0;
done:	return r;
}

static int
initCrypt(FILE *eifp, int gcry_mode, char *key)
{
	#define GCRY_CIPHER GCRY_CIPHER_3DES  // TODO: make configurable
 	int r = 0;
	gcry_error_t     gcryError;
	char iv[4096];

	blkLength = gcry_cipher_get_algo_blklen(GCRY_CIPHER);
	if(blkLength > sizeof(iv)) {
		fprintf(stderr, "internal error[%s:%d]: block length %d too large for "
			"iv buffer\n", __FILE__, __LINE__, blkLength);
		r = 1; goto done;
	}
	if((r = eiGetIV(eifp, iv, blkLength)) != 0) goto done;

	size_t keyLength = gcry_cipher_get_algo_keylen(GCRY_CIPHER);
	if(strlen(key) != keyLength) {
		fprintf(stderr, "invalid key length; key is %u characters, but "
			"exactly %u characters are required\n", strlen(key),
			keyLength);
		r = 1; goto done;
	}

	gcryError = gcry_cipher_open(&gcry_chd, GCRY_CIPHER, gcry_mode, 0);
	if (gcryError) {
		printf("gcry_cipher_open failed:  %s/%s\n",
			gcry_strsource(gcryError),
			gcry_strerror(gcryError));
		r = 1; goto done;
	}

	gcryError = gcry_cipher_setkey(gcry_chd, key, keyLength);
	if (gcryError) {
		printf("gcry_cipher_setkey failed:  %s/%s\n",
			gcry_strsource(gcryError),
			gcry_strerror(gcryError));
		r = 1; goto done;
	}

	gcryError = gcry_cipher_setiv(gcry_chd, iv, blkLength);
	if (gcryError) {
		printf("gcry_cipher_setiv failed:  %s/%s\n",
			gcry_strsource(gcryError),
			gcry_strerror(gcryError));
		r = 1; goto done;
	}
done: return r;
}

static inline void
removePadding(char *buf, size_t *plen)
{
	unsigned len = (unsigned) *plen;
	unsigned iSrc, iDst;
	char *frstNUL;

	frstNUL = memchr(buf, 0x00, *plen);
	if(frstNUL == NULL)
		goto done;
	iDst = iSrc = frstNUL - buf;

	while(iSrc < len) {
		if(buf[iSrc] != 0x00)
			buf[iDst++] = buf[iSrc];
		++iSrc;
	}

	*plen = iDst;
done:	return;
}

static void
decryptBlock(FILE *fpin, FILE *fpout, off64_t blkEnd, off64_t *pCurrOffs)
{
	gcry_error_t     gcryError;
	size_t nRead, nWritten;
	size_t toRead;
	size_t nPad;
	size_t leftTillBlkEnd;
	char buf[64*1024];
	
	leftTillBlkEnd = blkEnd - *pCurrOffs;
	while(1) {
		toRead = sizeof(buf) <= leftTillBlkEnd ? sizeof(buf) : leftTillBlkEnd;
		toRead = toRead - toRead % blkLength;
		nRead = fread(buf, 1, toRead, fpin);
		if(nRead == 0)
			break;
		leftTillBlkEnd -= nRead, *pCurrOffs += nRead;
		nPad = (blkLength - nRead % blkLength) % blkLength;
		gcryError = gcry_cipher_decrypt(
				gcry_chd, // gcry_cipher_hd_t
				buf,    // void *
				nRead,    // size_t
				NULL,    // const void *
				0);   // size_t
		if (gcryError) {
			fprintf(stderr, "gcry_cipher_encrypt failed:  %s/%s\n",
			gcry_strsource(gcryError),
			gcry_strerror(gcryError));
			return;
		}
		removePadding(buf, &nRead);
		nWritten = fwrite(buf, 1, nRead, fpout);
		if(nWritten != nRead) {
			perror("fpout");
			return;
		}
	}
}


static int
doDecrypt(FILE *logfp, FILE *eifp, FILE *outfp, char *key)
{
	off64_t blkEnd;
	off64_t currOffs = 0;
	int r;

	while(1) {
		/* process block */
		if(initCrypt(eifp, GCRY_CIPHER_MODE_CBC, key) != 0)
			goto done;
		if((r = eiGetEND(eifp, &blkEnd)) != 0) goto done;
		decryptBlock(logfp, outfp, blkEnd, &currOffs);
		gcry_cipher_close(gcry_chd);
	}
	r = 0;
done:	return r;
}

static void
decrypt(char *name, char *key)
{
	FILE *logfp = NULL, *eifp = NULL;
	int r = 0;
	char eifname[4096];
	
	if(!strcmp(name, "-")) {
		fprintf(stderr, "decrypt mode cannot work on stdin\n");
		goto err;
	} else {
		if((logfp = fopen(name, "r")) == NULL) {
			perror(name);
			goto err;
		}
		snprintf(eifname, sizeof(eifname), "%s%s", name, ENCINFO_SUFFIX);
		eifname[sizeof(eifname)-1] = '\0';
		if((eifp = fopen(eifname, "r")) == NULL) {
			perror(eifname);
			goto err;
		}
		if(eiCheckFiletype(eifp) != 0)
			goto err;
	}

	doDecrypt(logfp, eifp, stdout, key);

	fclose(logfp); logfp = NULL;
	fclose(eifp); eifp = NULL;
	return;

err:
	fprintf(stderr, "error %d processing file %s\n", r, name);
	if(logfp != NULL)
		fclose(logfp);
}


static struct option long_options[] = 
{ 
	{"verbose", no_argument, NULL, 'v'},
	{"version", no_argument, NULL, 'V'},
	{"decrypt", no_argument, NULL, 'd'},
	{"key", required_argument, NULL, 'k'},
	{NULL, 0, NULL, 0} 
}; 

int
main(int argc, char *argv[])
{
	int i;
	int opt;
	char *key = "";

	while(1) {
		opt = getopt_long(argc, argv, "dk:vV", long_options, NULL);
		if(opt == -1)
			break;
		switch(opt) {
		case 'd':
			mode = MD_DECRYPT;
			break;
		case 'k':
			fprintf(stderr, "WARNING: specifying the actual key "
				"via the command line is highly insecure\n"
				"Do NOT use this for PRODUCTION use.\n");
			key = optarg;
			break;
		case 'v':
			verbose = 1;
			break;
		case 'V':
			fprintf(stderr, "rsgtutil " VERSION "\n");
			exit(0);
			break;
		case '?':
			break;
		default:fprintf(stderr, "getopt_long() returns unknown value %d\n", opt);
			return 1;
		}
	}

	if(optind == argc)
		decrypt("-", key);
	else {
		for(i = optind ; i < argc ; ++i)
			decrypt(argv[i], key); /* currently only mode ;) */
	}

	memset(key, 0, strlen(key)); /* zero-out key store */
	return 0;
}
	//char *aesSymKey = "123456789012345678901234"; // TODO: TEST ONLY
