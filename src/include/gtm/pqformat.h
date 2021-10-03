/*-------------------------------------------------------------------------
 *
 * pqformat.h
 *		Definitions for formatting and parsing frontend/backend messages
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2010-2012 Postgres-XC Development Group
 *
 * $PostgreSQL: pgsql/src/include/libpq/pqformat.h,v 1.27 2009/01/01 17:23:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PQFORMAT_H
#define PQFORMAT_H

#include "gtm/stringinfo.h"

extern void gtm_pq_beginmessage(StringInfo buf, char msgtype);
extern void pq_sendbyte(StringInfo buf, int byt);
extern void gtm_pq_sendbytes(StringInfo buf, const char *data, int datalen);
extern void gtm_pq_sendcountedtext(StringInfo buf, const char *str, int slen,
				   bool countincludesself);
extern void gtm_pq_sendtext(StringInfo buf, const char *str, int slen);
extern void gtm_pq_sendstring(StringInfo buf, const char *str);
extern void gtm_pq_send_ascii_string(StringInfo buf, const char *str);
extern void pq_sendint(StringInfo buf, int i, int b);
extern void pq_sendint64(StringInfo buf, int64 i);
extern void gtm_pq_sendfloat4(StringInfo buf, float4 f);
extern void gtm_pq_sendfloat8(StringInfo buf, float8 f);
extern void gtm_pq_endmessage(Port *myport, StringInfo buf);

extern void gtm_pq_puttextmessage(Port *myport, char msgtype, const char *str);
extern void gtm_pq_putemptymessage(Port *myport, char msgtype);

extern int	gtm_pq_getmsgbyte(StringInfo msg);
extern unsigned int gtm_pq_getmsgint(StringInfo msg, int b);
extern int64 gtm_pq_getmsgint64(StringInfo msg);
extern float4 gtm_pq_getmsgfloat4(StringInfo msg);
extern float8 gtm_pq_getmsgfloat8(StringInfo msg);
extern const char *gtm_pq_getmsgbytes(StringInfo msg, int datalen);
extern void gtm_pq_copymsgbytes(StringInfo msg, char *buf, int datalen);
extern char *gtm_pq_getmsgtext(StringInfo msg, int rawbytes, int *nbytes);
extern const char *gtm_pq_getmsgstring(StringInfo msg);
extern void gtm_pq_getmsgend(StringInfo msg);
extern int pq_getmsgunreadlen(StringInfo msg);

#endif   /* PQFORMAT_H */
