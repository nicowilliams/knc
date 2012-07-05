/* $Id: gssstdio.c,v 1.6 2010/04/14 11:26:50 dowdes Exp $ */

/*-
 * Copyright 2009  Morgan Stanley and Co. Incorporated
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*-
 * Copyright (c) 2003 Roland C. Dowdeswell.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#include <sys/socket.h>

#include <netinet/in.h>

/* #include <netdb.h> */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <syslog.h>
#include <unistd.h>

/* this include must be before krb5/resolve.conf for things to work */
#include <arpa/nameser.h>

extern char _log_buff[2048];

#include "gssstdio.h"
#include "knc.h"

/* The rest of them are internal utility functions */

static int	write_packet(int, gss_buffer_t);
static int	read_packet(int, gss_buffer_t);
static int	gstd_errstring(char **, OM_uint32, gss_OID, OM_uint32);

#define SETUP_GSTD_TOK(s,ctx,mech,fd,log) do {				\
		(s) = malloc(sizeof(*(s)));				\
		if (!(s)) {						\
			LOG(LOG_ERR, ("%s: could not malloc(3), %s",	\
				      (log), strerror(errno)));		\
			return NULL;					\
		}							\
		(s)->gstd_ctx = (ctx);					\
		(s)->gstd_mech = (mech);				\
		(s)->gstd_inbuf.length = 0;				\
		(s)->gstd_inbuf.value = NULL;				\
		(s)->gstd_inbufpos = -1;				\
		(s)->gstd_fd  = (fd);					\
	} while (0)

#define FREE_NOTNULL(x)	if (x) free(x)

static
char *
gstd_get_display_name(gss_name_t client, gss_OID mech) {
	OM_uint32	maj;
	OM_uint32	min;
	gss_buffer_desc	buf;
	char		*ret;

	maj = gss_display_name(&min, client, &buf, NULL);
	GSTD_GSS_ERROR(maj, min, mech, NULL, "gss_display_name");

	if ((ret = (char *)malloc(buf.length + 1)) == NULL) {
		LOG(LOG_ERR, ("unable to malloc"));
		gss_release_buffer(&min, &buf);
		return NULL;
	}

	memcpy(ret, buf.value, buf.length);
	ret[buf.length] = '\0';

	gss_release_buffer(&min, &buf);

	return ret;
}

void *
gstd_accept(int fd, char **display_creds)
{
	gss_name_t client;
	struct gstd_tok *tok;
	gss_ctx_id_t	 ctx = GSS_C_NO_CONTEXT;
	gss_buffer_desc	 in, out;
	OM_uint32	 maj, min;
        gss_OID		 mech;
	int		 ret;

	out.length = 0;
	in.length = 0;
again:
	while ((ret = read_packet(fd, &in)) == -2)
		;

	if (ret < 1)
		return NULL;

	maj = gss_accept_sec_context(&min, &ctx, GSS_C_NO_CREDENTIAL,
	    &in, GSS_C_NO_CHANNEL_BINDINGS, &client, &mech, &out, NULL,
	    NULL, NULL);

	if (out.length && write_packet(fd, &out)) {
		gss_release_buffer(&min, &out);
		return NULL;
	}

	GSTD_GSS_ERROR(maj, min, mech, NULL, "gss_accept_sec_context");

	if (maj & GSS_S_CONTINUE_NEEDED)
		goto again;

	*display_creds = gstd_get_display_name(client, mech);
	gss_release_name(&min, &client);
	SETUP_GSTD_TOK(tok, ctx, mech, fd, "gstd_accept");
	return tok;
}


void *
gstd_initiate(const char *hostname, const char *service, const char *princ,
	      int fd)
{
	struct gstd_tok	*tok;
	gss_ctx_id_t	ctx = GSS_C_NO_CONTEXT;
	gss_buffer_desc	in, out;
	gss_OID		type;
        /* XXXnico have desired_mech passed in */
	gss_OID		desired_mech = GSS_C_NO_OID;
	gss_OID		actual_mech;
	OM_uint32	maj, min;
	gss_buffer_desc	name;
	gss_name_t	server;
	int		ret;

	if (!princ) {
		if ((name.value = malloc(strlen(service) + strlen(hostname)
					 + 2)) == NULL) {
			LOG(LOG_ERR, ("unable to malloc service name"));
			return NULL;
		}

		name.length = sprintf((char *)name.value, "%s@%s",
				      service, hostname);
		LOG(LOG_DEBUG, ("going to get tickets for: %s",
		    (char *)name.value));
		fflush(stderr);
		if (!name.value)
			return NULL;
		type = GSS_C_NT_HOSTBASED_SERVICE;
	} else {
		name.value = (char *) princ;
		name.length = strlen(princ);
		type = (gss_OID) GSS_C_NO_OID;
	}

	maj = gss_import_name(&min, &name, type, &server);
	GSTD_GSS_ERROR(maj, min, GSS_C_NO_OID, NULL, "gss_import_name");

	in.length = 0;
	out.length = 0;

again:
	maj = gss_init_sec_context(&min, GSS_C_NO_CREDENTIAL, &ctx, server,
	    desired_mech, GSS_C_MUTUAL_FLAG | GSS_C_SEQUENCE_FLAG, 0,
	    GSS_C_NO_CHANNEL_BINDINGS, &in, &actual_mech, &out, NULL, NULL);

	if (out.length && write_packet(fd, &out))
		return NULL;

	GSTD_GSS_ERROR(maj, min,
		       (actual_mech != GSS_C_NO_OID) ?
			   actual_mech : desired_mech,
		       NULL, "gss_init_sec_context");

	if (GSS_ERROR(maj) && ctx != GSS_C_NO_CONTEXT) {
		gss_delete_sec_context(&min, &ctx, GSS_C_NO_BUFFER);
		return NULL;
	}

	if (maj & GSS_S_CONTINUE_NEEDED) {
		LOG(LOG_DEBUG, ("continuing gstd_initiate"));
		while ((ret = read_packet(fd, &in)) == -2)
			;

		if (ret < 1) {
			LOG(LOG_ERR, ("continuation failed"));
			return NULL;
		}

		goto again;
	}

	LOG(LOG_DEBUG, ("authenticated"));
	SETUP_GSTD_TOK(tok, ctx, actual_mech, fd, "gstd_connect");
	return tok;
}

#ifdef MIN
#undef MIN
#endif
#define MIN(a, b) ((a) < (b) ? (a) : (b))
int
gstd_read(void *the_tok, char *buf, int length)
{
	struct gstd_tok	*tok = the_tok;
	gss_buffer_desc	in;
	OM_uint32	maj, min;
	int		bufpos = tok->gstd_inbufpos;
	int		ret;

	/*
	 * If we have no buffered data, read another packet and
	 * reset the buffer.
	 */

	if (bufpos == -1 || bufpos >= tok->gstd_inbuf.length) {
		if (tok->gstd_inbuf.length > 0)
			gss_release_buffer(&min, &tok->gstd_inbuf);

		/*
		 * If we encounter a protocol botch or if the other side has
		 * closed the connection, we return that fact here
		 */
		ret = read_packet(tok->gstd_fd, &in);
		if (ret <= 0)
			return ret;

		maj = gss_unwrap(&min, tok->gstd_ctx, &in, &tok->gstd_inbuf,
		    NULL, NULL);
		GSTD_GSS_STREAM_ERROR(maj, min, tok->gstd_mech, -1, "gss_unwrap");
		gss_release_buffer(&min, &in);
		bufpos = 0;
	}

	/*
	 * Now we know that we have a buffered packet, so return
	 * as much of it as we can.  We do not need to fill the
	 * requestor's buffer, because stdio can deal with short
	 * reads.
	 */

	length = MIN(length, tok->gstd_inbuf.length - bufpos);
	memcpy(buf, tok->gstd_inbuf.value + bufpos, length);
	tok->gstd_inbufpos = bufpos + length;
	LOG(LOG_DEBUG, ("read %d bytes", length));
	return length;
}

#if 0
int
gstd_write(work_t *work)
{
	struct gstd_tok	*tok = work->the_tok;
	gss_buffer_desc	in, out;
	OM_uint32	maj, min;

	/*
	 * We clip the length at GSTD_MAXPACKETCONTENTS (+fudge) to make
	 * the job of the receiver easier.
	 */

	if (length <= 0)	/* hmmm, error eh? */
		return -1;

	if (length > GSTD_MAXPACKETCONTENTS)
		length = GSTD_MAXPACKETCONTENTS;

	in.length = work->network_buffer.len;
	in.value  = (void *)work->network_buffer.buffer;

	maj = gss_wrap(&min, tok->gstd_ctx, 1, GSS_C_QOP_DEFAULT,
	    &in, NULL, &out);
	GSTD_GSS_ERROR(maj, min, tok->gstd_mech, -1, "gss_wrap");

	/* should I loop on this one? */
	if (write_packet(tok->gstd_fd, &out))
		return -1;

	LOG(LOG_DEBUG, ("wrote %d bytes", length));
	return length;
}

#endif

int
gstd_close(void *the_tok)
{
	struct gstd_tok	*tok = the_tok;
	OM_uint32	 min;

	gss_delete_sec_context(&min, &tok->gstd_ctx, GSS_C_NO_BUFFER);
	if (tok->gstd_inbuf.length > 0)
		gss_release_buffer(&min, &tok->gstd_inbuf);
	close(tok->gstd_fd);
	return 0;
}

/*
 * Returns:
 *	-2	Need to call again
 *	-1	Protocol error
 *	0	Normal EOF (non-protocol error, other side is finished and
 *		has simply closed the connection)
 *	1       Data has been completely received
 */
static int
read_packet(int fd, gss_buffer_t buf)
{
	int	  ret;

	static uint32_t		len = 0;
	static char		len_buf[4];
	static int		len_buf_pos = 0;
	static char *		tmpbuf = 0;
	static int		tmpbuf_pos = 0;

	if (len_buf_pos < 4) {
		ret = read(fd, &len_buf[len_buf_pos], 4 - len_buf_pos);

		if (ret == -1) {
			if (errno == EINTR || errno == EAGAIN)
				return -2;

			LOG(LOG_ERR, ("%s", strerror(errno)));
			return -1;
		}

		if (ret == 0) {		/* EOF */
			/* Failure to read ANY length just means we're done */
			if (len_buf_pos == 0)
				return 0;

			/*
			 * Otherwise, we got EOF mid-length, and that's
			 * a protocol error.
			 */
			LOG(LOG_INFO, ("EOF reading packet len"));
			return -1;
		}

		len_buf_pos += ret;
	}

	/* Not done reading the length? */
	if (len_buf_pos != 4)
		return -2;

	/* We have the complete length */
	len = ntohl(*(uint32_t *)len_buf);

	/*
	 * We make sure recvd length is reasonable, allowing for some
	 * slop in enc overhead, beyond the actual maximum number of
	 * bytes of decrypted payload.
	 */
	if (len > GSTD_MAXPACKETCONTENTS + 512) {
		LOG(LOG_ERR, ("ridiculous length, %ld", len));
		return -1;
	}

	if (!tmpbuf) {
		if ((tmpbuf = malloc(len)) == NULL) {
			LOG(LOG_CRIT, ("malloc failure, %ld bytes", len));
			return -1;
		}
	}

	ret = read(fd, tmpbuf + tmpbuf_pos, len - tmpbuf_pos);
	if (ret == -1) {
		if (errno == EINTR || errno == EAGAIN)
			return -2;
		
		LOG(LOG_ERR, ("%s", strerror(errno)));
		return -1;
	}

	if (ret == 0) {
		LOG(LOG_ERR, ("EOF while reading packet (len=%d)", len));
		return -1;
	}

	tmpbuf_pos += ret;

	if (tmpbuf_pos == len) {
		buf->length = len;
		buf->value = tmpbuf;
		len = len_buf_pos = tmpbuf_pos = 0;
		tmpbuf = NULL;

		LOG(LOG_DEBUG, ("read packet of length %d", buf->length));
		return 1;
	}

	return -2;
}

static int
write_packet(int fd, gss_buffer_t buf)
{
	uint32_t	len;
	OM_uint32	min_stat;
	int		ret = 0;

	len = htonl(buf->length);
	if ((writen(fd, &len, 4) != 4) ||
	    (writen(fd, buf->value, buf->length) != buf->length))
		ret = -1;

	gss_release_buffer (&min_stat, buf);
	return ret;
}


/*
 * The following function writes up to len bytes, returning -1 if it fails
 * to do so for any reason, and len otherwise.  Note, partial writes may
 * have occurred if this function returns -1
 */
int
writen(int fd, const void *buf, ssize_t len) {
	int	nleft;
	int	nwritten;
	char *	buffer = (char *)buf;

	nleft = len;
	while (nleft > 0) {
		nwritten = write(fd, buffer, len);

		if (nwritten < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			else {
				LOG_ERRNO(LOG_ERR, ("write failed"));
				return -1;
			}
		} else {
			nleft -= nwritten;
			buffer += nwritten;
		}
	}

	LOG(LOG_DEBUG, ("wrote %d bytes", len));
	return len;
}


static int
gstd_errstring(char **str, OM_uint32 maj, gss_OID mech, OM_uint32 min)
{
	gss_buffer_desc	 status;
	OM_uint32	 new_stat;
	OM_uint32	 msg_ctx = 0;
	OM_uint32	 ret;
	int		 len = 0;
	char		*tmp;
	char		*statstr;

	if (!str)
		return -1;

	*str = NULL;
	tmp = NULL;

	do {
		ret = gss_display_status(&new_stat, min,
		    GSS_C_MECH_CODE, GSS_C_NO_OID, &msg_ctx,
		    &status);

		/* GSSAPI strings are not NUL terminated */
		if ((statstr = (char *)malloc(status.length + 1)) == NULL) {
			LOG(LOG_ERR, ("unable to malloc status string "
				      "of length %ld", status.length));
			gss_release_buffer(&new_stat, &status);
			free(statstr);
			free(tmp);
			return 0;
		}

		memcpy(statstr, status.value, status.length);
		statstr[status.length] = '\0';

		if (GSS_ERROR(ret)) {
			free(statstr);
			free(tmp);
			break;
		}

		if (*str) {
			if ((*str = malloc(strlen(*str) + status.length +
					   3)) == NULL) {
				LOG(LOG_ERR, ("unable to malloc error "
						"string"));
				gss_release_buffer(&new_stat, &status);
				free(statstr);
				free(tmp);
				return 0;
			}

			len = sprintf(*str, "%s, %s", tmp, statstr);
		} else {
			*str = malloc(status.length + 1);
			len = sprintf(*str, "%s", (char *)statstr);
		}

		gss_release_buffer(&new_stat, &status);
		free(statstr);
		free(tmp);

		tmp = *str;
	} while (msg_ctx != 0);

	return len;
}

/* XXXnico still need to make sure we pass a real mech OID to this */
void
gstd_error(int pri, OM_uint32 maj, gss_OID mech, OM_uint32 min, const char *s)
{
	char *t1;

	if (gstd_errstring(&t1, maj, mech, min) < 1)
		LOG(pri, ("%s: couldn't form GSSAPI error string", s));
	else {
		LOG(pri, ("%s: %s", s, t1));
		free(t1);
	}
}

void
gstd_release_context(void *ctx) {
	OM_uint32	min;

	gss_delete_sec_context(&min, (gss_ctx_id_t *)ctx, GSS_C_NO_BUFFER);
}
