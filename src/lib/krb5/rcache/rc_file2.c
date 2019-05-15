/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/* lib/krb5/rcache/rc_file2.c - file-based replay cache, version 2 */
/*
 * Copyright (C) 2019 by the Massachusetts Institute of Technology.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in
 *   the documentation and/or other materials provided with the
 *   distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "k5-int.h"
#include "k5-hashtab.h"
#include "rc-int.h"
#ifndef _WIN32
#include <sys/types.h>
#include <sys/stat.h>
#endif

#define MAX_SIZE INT32_MAX
#define TAG_LEN 12
#define RECORD_LEN (TAG_LEN + 4)
#define FIRST_TABLE_RECORDS 1023

/* Return the offset and number of records in the next table.  *offset should
 * initially be -1. */
static inline krb5_error_code
next_table(off_t *offset, off_t *nrecords)
{
    if (*offset == -1) {
        *offset = K5_HASH_SEED_LEN;
        *nrecords = FIRST_TABLE_RECORDS;
    } else if (*offset == K5_HASH_SEED_LEN) {
        *offset += *nrecords * RECORD_LEN;
        *nrecords = (FIRST_TABLE_RECORDS + 1) * 2;
    } else {
        *offset += *nrecords * RECORD_LEN;
        *nrecords *= 2;
    }

    /* Make sure the next table fits within the maximum file size. */
    if (*nrecords > MAX_SIZE / RECORD_LEN)
        return EOVERFLOW;
    if (*offset > MAX_SIZE - (*nrecords * RECORD_LEN))
        return EOVERFLOW;

    return 0;
}

/* Read up to two records from fd at offset, and parse them out into tags and
 * timestamps.  Place the number of records read in *nread. */
static krb5_error_code
read_records(int fd, off_t offset, uint8_t tag1_out[TAG_LEN],
             uint32_t *timestamp1_out, uint8_t tag2_out[TAG_LEN],
             uint32_t *timestamp2_out, int *nread)
{
    uint8_t buf[RECORD_LEN * 2];
    ssize_t st;

    *nread = 0;

    st = lseek(fd, offset, SEEK_SET);
    if (st == -1)
        return errno;
    st = read(fd, buf, RECORD_LEN * 2);
    if (st == -1)
        return errno;

    if (st >= RECORD_LEN) {
        memcpy(tag1_out, buf, TAG_LEN);
        *timestamp1_out = load_32_be(buf + TAG_LEN);
        *nread = 1;
    }
    if (st == RECORD_LEN * 2) {
        memcpy(tag2_out, buf + RECORD_LEN, TAG_LEN);
        *timestamp2_out = load_32_be(buf + RECORD_LEN + TAG_LEN);
        *nread = 2;
    }
    return 0;
}

/* Write one record to fd at offset, marshalling the tag and timestamp. */
static krb5_error_code
write_record(int fd, off_t offset, const uint8_t tag[TAG_LEN],
             uint32_t timestamp)
{
    uint8_t record[RECORD_LEN];
    ssize_t st;

    memcpy(record, tag, TAG_LEN);
    store_32_be(timestamp, record + TAG_LEN);

    st = lseek(fd, offset, SEEK_SET);
    if (st == -1)
        return errno;
    st = write(fd, record, RECORD_LEN);
    if (st == -1)
        return errno;
    if (st != RECORD_LEN) /* Unexpected for a regular file */
        return EIO;

    return 0;
}

/* Check and store a record into an open and locked file.  fd is assumed to be
 * at offset 0. */
static krb5_error_code
store(krb5_context context, int fd, const uint8_t tag[TAG_LEN], uint32_t now,
      uint32_t skew)
{
    krb5_error_code ret;
    krb5_data d;
    off_t table_offset = -1, nrecords = 0, avail_offset = -1, record_offset;
    ssize_t st;
    int ind, nread;
    uint8_t seed[K5_HASH_SEED_LEN], rec1_tag[TAG_LEN], rec2_tag[TAG_LEN];
    uint32_t rec1_stamp, rec2_stamp;

    /* Read or generate the hash seed. */
    st = read(fd, seed, sizeof(seed));
    if (st < 0)
        return errno;
    if ((size_t)st < sizeof(seed)) {
        d = make_data(seed, sizeof(seed));
        ret = krb5_c_random_make_octets(context, &d);
        if (ret)
            return ret;
        st = write(fd, seed, sizeof(seed));
        if (st < 0)
            return errno;
        if ((size_t)st != sizeof(seed))
            return EIO;
    }

    for (;;) {
        ret = next_table(&table_offset, &nrecords);
        if (ret)
            return ret;

        ind = k5_siphash24(tag, TAG_LEN, seed) % nrecords;
        record_offset = table_offset + ind * RECORD_LEN;

        ret = read_records(fd, record_offset, rec1_tag, &rec1_stamp, rec2_tag,
                           &rec2_stamp, &nread);
        if (ret)
            return ret;

        if ((nread >= 1 && memcmp(rec1_tag, tag, TAG_LEN) == 0) ||
            (nread == 2 && memcmp(rec2_tag, tag, TAG_LEN) == 0))
            return KRB5KRB_AP_ERR_REPEAT;

        if (avail_offset == -1) {
            if (nread == 0 || ts_after(now, ts_incr(rec1_stamp, skew)))
                avail_offset = record_offset;
            else if (nread == 1 || ts_after(now, ts_incr(rec2_stamp, skew)))
                avail_offset = record_offset + RECORD_LEN;
        }

        if (nread < 2 || rec1_stamp == 0 || rec2_stamp == 0)
            return write_record(fd, avail_offset, tag, now);

        /* Use a different hash seed for the next table we search. */
        seed[0]++;
    }
}

krb5_error_code
k5_rcfile2_store(krb5_context context, int fd, krb5_donot_replay *rep)
{
    krb5_error_code ret;
    krb5_timestamp now;
    uint8_t tag[TAG_LEN];

    if (rep->tag.length == 0)
        return EINVAL;

    ret = krb5_timeofday(context, &now);
    if (ret)
        return ret;

    if (rep->tag.length >= TAG_LEN) {
        memcpy(tag, rep->tag.data, TAG_LEN);
    } else {
        memcpy(tag, rep->tag.data, rep->tag.length);
        memset(tag + rep->tag.length, 0, TAG_LEN - rep->tag.length);
    }

    ret = krb5_lock_file(context, fd, KRB5_LOCKMODE_EXCLUSIVE);
    if (ret)
        return ret;
    ret = store(context, fd, tag, now, context->clockskew);
    (void)krb5_unlock_file(NULL, fd);
    return ret;
}

static char * KRB5_CALLCONV
file2_get_name(krb5_context context, krb5_rcache rc)
{
    return (char *)rc->data;
}

static krb5_error_code KRB5_CALLCONV
file2_get_span(krb5_context context, krb5_rcache rc, krb5_deltat *lifespan)
{
    *lifespan = context->clockskew;
    return 0;
}

static krb5_error_code KRB5_CALLCONV
file2_init(krb5_context context, krb5_rcache rc, krb5_deltat lifespan)
{
    return 0;
}

static krb5_error_code KRB5_CALLCONV
file2_close(krb5_context context, krb5_rcache rc)
{
    k5_mutex_destroy(&rc->lock);
    free(rc->data);
    free(rc);
    return 0;
}

#define file2_destroy file2_close

static krb5_error_code KRB5_CALLCONV
file2_resolve(krb5_context context, krb5_rcache rc, char *name)
{
    rc->data = strdup(name);
    return (rc->data == NULL) ? ENOMEM : 0;
}

static krb5_error_code KRB5_CALLCONV
file2_recover(krb5_context context, krb5_rcache rc)
{
    return 0;
}

static krb5_error_code KRB5_CALLCONV
file2_recover_or_init(krb5_context context, krb5_rcache rc,
                      krb5_deltat lifespan)
{
    return 0;
}

static krb5_error_code KRB5_CALLCONV
file2_store(krb5_context context, krb5_rcache rc, krb5_donot_replay *rep)
{
    krb5_error_code ret;
    const char *filename = rc->data;
    int fd;

    fd = open(filename, O_CREAT | O_RDWR | O_BINARY, 0600);
    if (fd < 0) {
        ret = errno;
        k5_setmsg(context, ret, "%s (filename: %s)", error_message(ret),
                  filename);
        return ret;
    }
    ret = k5_rcfile2_store(context, fd, rep);
    close(fd);
    return ret;
}

static krb5_error_code KRB5_CALLCONV
file2_expunge(krb5_context context, krb5_rcache rc)
{
    return 0;
}

const krb5_rc_ops krb5_rc_file2_ops =
{
    0,
    "file2",
    file2_init,
    file2_recover,
    file2_recover_or_init,
    file2_destroy,
    file2_close,
    file2_store,
    file2_expunge,
    file2_get_span,
    file2_get_name,
    file2_resolve
};