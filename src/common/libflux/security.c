/*****************************************************************************\
 *  Copyright (c) 2014 Lawrence Livermore National Security, LLC.  Produced at
 *  the Lawrence Livermore National Laboratory (cf, AUTHORS, DISCLAIMER.LLNS).
 *  LLNL-CODE-658032 All rights reserved.
 *
 *  This file is part of the Flux resource manager framework.
 *  For details, see https://github.com/flux-framework.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the license, or (at your option)
 *  any later version.
 *
 *  Flux is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the terms and conditions of the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *  See also:  http://www.gnu.org/licenses/
\*****************************************************************************/

/* security.c - flux security functions */

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <getopt.h>
#include <libgen.h>
#include <munge.h>
#include <pwd.h>
#include <czmq.h>

#include "security.h"
#include "flog.h"

#include "src/common/libutil/log.h"
#include "src/common/libutil/oom.h"
#include "src/common/libutil/xzmalloc.h"
#include "src/common/libutil/base64.h"


#define FLUX_ZAP_DOMAIN "flux"

struct flux_sec_struct {
    zactor_t *auth;
    int typemask;
    zcert_t *srv_cert;
    zcert_t *cli_cert;
    munge_ctx_t mctx;
    char *conf_dir;
    char *curve_dir;
    char *passwd_file;
    char *errstr;
    char *confstr;
    uid_t uid;
    uid_t gid;
    char *principal;
};

static int checksecdirs (flux_sec_t *c, bool create);
static zcert_t *getcurve (flux_sec_t *c, const char *role);
static int gencurve (flux_sec_t *c, const char *role);
static char *getpasswd (flux_sec_t *c, const char *user);
static int genpasswd (flux_sec_t *c, const char *user);

const char *flux_sec_errstr (flux_sec_t *c)
{
    return (c->errstr ? c->errstr : "Success");
}

const char *flux_sec_confstr (flux_sec_t *c)
{
    if (c->confstr)
        free (c->confstr);
    if (asprintf (&c->confstr, "Security: epgm=%s, tcp/ipc=%s",
               (c->typemask & FLUX_SEC_TYPE_MUNGE) ? "MUNGE" : "off",
               (c->typemask & FLUX_SEC_TYPE_PLAIN) ? "PLAIN"
             : (c->typemask & FLUX_SEC_TYPE_CURVE) ? "CURVE"
             : (c->typemask & FLUX_SEC_TYPE_GSSAPI) ? "GSSAPI" : "off") < 0)
        oom ();
    return c->confstr;
}

static void seterrstr (flux_sec_t *c, const char *fmt, ...)
{
    va_list ap;

    if (c->errstr)
        free (c->errstr);
    va_start (ap, fmt);
    if (vasprintf (&c->errstr, fmt, ap) < 0)
        oom ();
    va_end (ap);
}

void flux_sec_destroy (flux_sec_t *c)
{
    if (c) {
        free (c->conf_dir);
        free (c->curve_dir);
        free (c->passwd_file);
        zcert_destroy (&c->cli_cert);
        zcert_destroy (&c->srv_cert);
        if (c->mctx)
            munge_ctx_destroy (c->mctx);
        free (c->errstr);
        free (c->confstr);
        zactor_destroy (&c->auth);
        free (c->principal);
        free (c);
    }
}

static char *lookup_username (uid_t uid)
{
    struct passwd pwd, *result;
    long bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
    char *buf = NULL;
    char *username = NULL;
    int e;

    if (bufsize == -1)
        bufsize = 16384;        /* Should be more than enough */
    if (!(buf = calloc (1, bufsize))) {
        errno = ENOMEM;
        goto error;
    }
    e = getpwuid_r (uid, &pwd, buf, bufsize, &result);
    if (result == NULL) {
        errno = e ? e : ENOENT;
        goto error;
    }
    if (!(username = strdup (result->pw_name))) {
        errno = ENOENT;
        goto error;
    }
    free (buf);
    return username;
error:
    free (buf);
    return NULL;
}


flux_sec_t *flux_sec_create (int typemask, const char *confdir)
{
    flux_sec_t *c = calloc (1, sizeof (*c));
    int count = 0;

    if (!c) {
        errno = ENOMEM;
        goto error;
    }
    if ((typemask & FLUX_SEC_TYPE_CURVE))
        count++;
    if ((typemask & FLUX_SEC_TYPE_PLAIN))
        count++;
    if ((typemask & FLUX_SEC_TYPE_GSSAPI))
        count++;
    if (count > 1) {
        errno = EINVAL;
        goto error;
    }
    if (confdir) {
        if (!(c->conf_dir = strdup (confdir))) {
            errno = ENOMEM;
            goto error;
        }
    }
    c->uid = getuid ();
    c->gid = getgid ();
    if (!(c->principal = lookup_username (c->uid)))
        goto error;
    c->typemask = typemask;
    return c;
error:
    flux_sec_destroy (c);
    return NULL;
}

const char *flux_sec_get_directory (flux_sec_t *c)
{
    return c->conf_dir;
}

bool flux_sec_type_enabled (flux_sec_t *c, int tm)
{
    bool ret;
    ret = ((c->typemask & tm) == tm);
    return ret;
}

int flux_sec_keygen (flux_sec_t *c)
{
    int rc = -1;
    if (checksecdirs (c, true) < 0)
        goto done;
    if ((c->typemask & FLUX_SEC_TYPE_CURVE)) {
        if (gencurve (c, "client") < 0)
            goto done;
        if (gencurve (c, "server") < 0)
            goto done;
    }
    if ((c->typemask & FLUX_SEC_TYPE_PLAIN)) {
        if (genpasswd (c, "client") < 0)
            goto done;
    }
    rc = 0;
done:
    return rc;
}

int flux_sec_comms_init (flux_sec_t *c)
{
    if (c->mctx == NULL && (c->typemask & FLUX_SEC_TYPE_MUNGE)
                        && !(c->typemask & FLUX_SEC_FAKEMUNGE)) {
        munge_err_t e;
        if (!(c->mctx = munge_ctx_create ())) {
            seterrstr (c, "munge_ctx_create: %s", flux_strerror (errno));
            goto error;
        }
        e = munge_ctx_set (c->mctx, MUNGE_OPT_UID_RESTRICTION, c->uid);
        if (e != EMUNGE_SUCCESS) {
            seterrstr (c, "munge_ctx_set: %s", munge_strerror (e));
            errno = EINVAL;
            goto error;
        }
    }
    if (c->auth == NULL && ((c->typemask & FLUX_SEC_TYPE_CURVE)
                        || (c->typemask & FLUX_SEC_TYPE_GSSAPI)
                        || (c->typemask & FLUX_SEC_TYPE_PLAIN))) {
        if (!(c->auth = zactor_new (zauth, NULL))) {
            seterrstr (c, "zactor_new (zauth): %s", flux_strerror (errno));
            goto error;
        }
        if ((c->typemask & FLUX_SEC_VERBOSE)) {
            if (zstr_sendx (c->auth, "VERBOSE", NULL) < 0)
                goto error;
            if (zsock_wait (c->auth) < 0)
                goto error;
        }
        if ((c->typemask & FLUX_SEC_TYPE_CURVE)) {
            if (!zsys_has_curve ()) {
                seterrstr (c, "libczmq was not built with CURVE support!");
                errno = EINVAL;
                goto error;
            }
            if (checksecdirs (c, false) < 0)
                goto error;
            if (!(c->cli_cert = getcurve (c, "client")))
                goto error;
            if (!(c->srv_cert = getcurve (c, "server")))
                goto error;
            /* Authorize only the clients with certs in $confdir/curve
             * (server must find public key of new client here)
             */
            if (zstr_sendx (c->auth, "CURVE", c->curve_dir, NULL) < 0)
                goto error;
            if (zsock_wait (c->auth) < 0)
                goto error;
        }
        else if ((c->typemask & FLUX_SEC_TYPE_PLAIN)) {
            if (checksecdirs (c, false) < 0)
                goto error;
            if (zstr_sendx (c->auth, "PLAIN", c->passwd_file, NULL) < 0)
                goto error;
            if (zsock_wait (c->auth) < 0)
                goto error;
        }
        else if ((c->typemask & FLUX_SEC_TYPE_GSSAPI)) {
            if (zstr_sendx (c->auth, "GSSAPI", NULL) < 0)
                goto error;
            if (zsock_wait (c->auth) < 0)
                goto error;
        }
    }
    return 0;
error:
    return -1;
}

int flux_sec_csockinit (flux_sec_t *c, void *sock)
{
    int rc = -1;

    if ((c->typemask & FLUX_SEC_TYPE_CURVE)) {
        zsock_set_zap_domain (sock, FLUX_ZAP_DOMAIN);
        zcert_apply (c->cli_cert, sock);
        zsock_set_curve_serverkey (sock, zcert_public_txt (c->srv_cert));
    }
    else if ((c->typemask & FLUX_SEC_TYPE_GSSAPI)) {
        zsock_set_gssapi_service_principal (sock, c->principal);
        zsock_set_gssapi_principal (sock, c->principal);
    }
    else if ((c->typemask & FLUX_SEC_TYPE_PLAIN)) {
        char *passwd = NULL;
        if (!(passwd = getpasswd (c, "client"))) {
            seterrstr (c, "client not found in %s", c->passwd_file);
            goto done;
        }
        zsock_set_plain_username (sock, "client");
        zsock_set_plain_password (sock, passwd);
        free (passwd);
    }
    rc = 0;
done:
    return rc;
}

int flux_sec_ssockinit (flux_sec_t *c, void *sock)
{
    if ((c->typemask & (FLUX_SEC_TYPE_CURVE))) {
        zsock_set_zap_domain (sock, FLUX_ZAP_DOMAIN);
        zcert_apply (c->srv_cert, sock);
        zsock_set_curve_server (sock, 1);
    }
    else if ((c->typemask & (FLUX_SEC_TYPE_GSSAPI))) {
        zsock_set_gssapi_server (sock, 1);
        zsock_set_gssapi_principal (sock, c->principal);
    }
    else if ((c->typemask & (FLUX_SEC_TYPE_PLAIN))) {
        zsock_set_plain_server (sock, 1);
    }
    return 0;
}

static int checksecdir (flux_sec_t *c, const char *path, bool create)
{
    struct stat sb;
    int rc = -1;

stat_again:
    if (lstat (path, &sb) < 0) {
        if (errno == ENOENT) {
            if (create) {
                if (mkdir (path, 0700) < 0) {
                    seterrstr (c, "mkdir %s: %s", path, strerror (errno));
                    goto done;
                }
                create = false;
                goto stat_again;
            } else {
                seterrstr (c, "The directory '%s' does not exist.  Have you run \"flux keygen\"?", path);
            }
        } else
            seterrstr (c, "lstat %s: %s", path, strerror (errno));
        goto done;
    }
    if (!S_ISDIR (sb.st_mode)) {
        errno = ENOTDIR;
        seterrstr (c, "%s: %s", path, strerror (errno));
        goto done;
    }
    if ((sb.st_mode & (S_IRWXU|S_IRWXG|S_IRWXO)) != 0700) {
        seterrstr (c, "%s: mode should be 0700", path);
        errno = EPERM;
        goto done;
    }
    if ((sb.st_uid != c->uid)) {
        seterrstr (c, "%s: owner should be you", path);
        errno = EPERM;
        goto done;
    }
    rc = 0;
done:
    return rc;
}

static int checksecdirs (flux_sec_t *c, bool create)
{
    if (!c->conf_dir) {
        seterrstr (c, "config directory is not set");
        errno = EINVAL;
        return -1;
    }
    if (checksecdir (c, c->conf_dir, create) < 0)
        return -1;
    if ((c->typemask & FLUX_SEC_TYPE_CURVE)) {
        if (!c->curve_dir) {
            if (asprintf (&c->curve_dir, "%s/curve", c->conf_dir) < 0) {
                errno = ENOMEM;
                return -1;
            }
        }
        if (checksecdir (c, c->curve_dir, create) < 0)
            return -1;
    }
    if ((c->typemask & FLUX_SEC_TYPE_PLAIN)) {
        if (!c->passwd_file) {
            if (asprintf (&c->passwd_file, "%s/passwd", c->conf_dir) < 0) {
                errno = ENOMEM;
                return -1;
            }
        }
    }
    return 0;
}

static char * ctime_iso8601_now (char *buf, size_t sz)
{
    struct tm tm;
    time_t now = time (NULL);

    memset (buf, 0, sz);

    if (!localtime_r (&now, &tm))
        return (NULL);
    strftime (buf, sz, "%FT%T", &tm);

    return (buf);
}

static zcert_t *zcert_curve_new (flux_sec_t *c)
{
    zcert_t *new;
    char sec[41];
    char pub[41];
    uint8_t s[32];
    uint8_t p[32];

    if (zmq_curve_keypair (pub, sec) < 0) {
        if (errno == ENOTSUP)
            seterrstr (c,
                "No CURVE support in libzmq (not compiled with libsodium?)");
        else
            seterrstr (c,
                "Unknown error generating CURVE keypair");
        return NULL;
    }

    if (!zmq_z85_decode (s, sec) || !zmq_z85_decode (p, pub)) {
        seterrstr (c, "zcert_curve_new: Failed to decode keys");
        return NULL;
    }

    if (!(new = zcert_new_from (p, s)))
        oom ();

    return new;
}

static int gencurve (flux_sec_t *c, const char *role)
{
    char *path = NULL, *priv = NULL;;
    zcert_t *cert = NULL;
    char buf[64];
    struct stat sb;
    int rc = -1;

    if (asprintf (&path, "%s/%s", c->curve_dir, role) < 0)
        oom ();
    if (asprintf (&priv, "%s/%s_private", c->curve_dir, role) < 0)
        oom ();
    if ((c->typemask & FLUX_SEC_KEYGEN_FORCE)) {
        (void)unlink (path);
        (void)unlink (priv);
    }
    if (stat (path, &sb) == 0) {
        seterrstr (c, "%s exists, try --force", path);
        errno = EEXIST;
        goto done;
    }
    if (stat (priv, &sb) == 0) {
        seterrstr (c, "%s exists, try --force", priv);
        errno = EEXIST;
        goto done;
    }

    if (!(cert = zcert_curve_new (c)))
        goto done; /* error message set in zcert_curve_new() */

    zcert_set_meta (cert, "time", "%s", ctime_iso8601_now (buf, sizeof (buf)));
    zcert_set_meta (cert, "role", "%s", role);
    if ((c->typemask & FLUX_SEC_VERBOSE)) {
        printf ("Saving %s\n", path);
        printf ("Saving %s\n", priv);
    }
    if (zcert_save (cert, path) < 0) {
        seterrstr (c, "zcert_save %s: %s", path, strerror (errno));
        goto done;
    }
    rc = 0;
done:
    if (cert)
        zcert_destroy (&cert);
    if (path)
        free (path);
    if (priv)
        free (priv);
    return rc;
}

static zcert_t *getcurve (flux_sec_t *c, const char *role)
{
    char s[PATH_MAX];
    zcert_t *cert = NULL;

    if (snprintf (s, sizeof (s), "%s/%s", c->curve_dir, role) >= sizeof (s)) {
        errno = EINVAL;
        goto error;
    }
    if (!(cert = zcert_load (s)))
        seterrstr (c, "zcert_load %s: %s", s, flux_strerror (errno));
    return cert;
error:
    return NULL;
}

static char *getpasswd (flux_sec_t *c, const char *user)
{
    zhash_t *passwds = NULL;
    const char *pass;
    char *s = NULL;

    if (!(passwds = zhash_new ())) {
        errno = ENOMEM;
        goto error;
    }
    zhash_autofree (passwds);
    if (zhash_load (passwds, c->passwd_file) < 0)
        goto error;
    if (!(pass = zhash_lookup (passwds, user))) {
        errno = ENOENT;
        goto error;
    }
    if (!(s = strdup (pass))) {
        errno = ENOMEM;
        goto error;
    }
    zhash_destroy (&passwds);
    return s;
error:
    zhash_destroy (&passwds);
    return NULL;
}

static int genpasswd (flux_sec_t *c, const char *user)
{
    struct stat sb;
    zhash_t *passwds = NULL;
    zuuid_t *uuid;
    mode_t old_mask;
    int rc = -1;

    if (!(uuid = zuuid_new ()))
        oom ();
    if ((c->typemask & FLUX_SEC_KEYGEN_FORCE))
        (void)unlink (c->passwd_file);
    if (stat (c->passwd_file, &sb) == 0) {
        seterrstr (c, "%s exists, try --force", c->passwd_file);
        errno = EEXIST;
        goto done;
    }
    if (!(passwds = zhash_new ()))
        oom ();
    zhash_update (passwds, user, (char *)zuuid_str (uuid));
    if ((c->typemask & FLUX_SEC_VERBOSE))
        printf ("Saving %s\n", c->passwd_file);
    old_mask = umask (077);
    rc = zhash_save (passwds, c->passwd_file);
    umask (old_mask);
    if (rc < 0) {
        seterrstr (c, "zhash_save %s: %s", c->passwd_file, flux_strerror (errno));
        goto done;
    }
    /* FIXME: check created file mode */
    rc = 0;
done:
    if (passwds)
        zhash_destroy (&passwds);
    if (uuid)
        zuuid_destroy (&uuid);
    return rc;
}

int flux_sec_munge (flux_sec_t *c, const char *inbuf, size_t insize,
                    char **outbuf, size_t *outsize)
{
    munge_err_t e;
    int rc = -1;

    if (!inbuf || !outbuf || !outsize || !c
                          || !(c->typemask & FLUX_SEC_TYPE_MUNGE)) {
        errno = EINVAL;
        return -1;
    }
    if ((c->typemask & FLUX_SEC_FAKEMUNGE)) {
        int dlen = base64_encode_length (insize);
        void *dst = xzmalloc (dlen);
        base64_encode_block (dst, &dlen, inbuf, insize);
        *outbuf = dst;
        *outsize = dlen;
    } else {
        if ((e = munge_encode (outbuf, c->mctx, inbuf,
                                                insize)) != EMUNGE_SUCCESS) {
            seterrstr (c, "munge_encode: %s", munge_strerror (e));
            errno = EKEYREJECTED;
            goto done;
        }
        *outsize = strlen (*outbuf) + 1; /* munge_decode needs null term */
    }
    rc = 0;
done:
    return rc;
}

int flux_sec_unmunge (flux_sec_t *c, const char *inbuf, size_t insize,
                      char **outbuf, size_t *outsize)
{
    munge_err_t e;
    int rc = -1;

    if (!c || !inbuf || !outbuf || !outsize
                     || !(c->typemask & FLUX_SEC_TYPE_MUNGE)) {
        errno = EINVAL;
        return -1;
    }
    if ((c->typemask & FLUX_SEC_FAKEMUNGE)) {
        int dlen = base64_decode_length (insize);
        void *dst = xzmalloc (dlen);
        if (base64_decode_block (dst, &dlen, inbuf, insize) < 0) {
            seterrstr (c, "munge_decode (fake) failed");
            free (dst);
            errno = EKEYREJECTED;
            goto done;
        }
        *outbuf = dst;
        *outsize = dlen;
    } else {
        if (inbuf[insize - 1] != '\0') {
            seterrstr (c, "munge cred is not null terminated");
            errno = EKEYREJECTED;
            goto done;
        }
        e = munge_decode (inbuf, c->mctx, (void **)outbuf, (int *)outsize,
                          NULL, NULL);
        if (e != EMUNGE_SUCCESS) {
            seterrstr (c, "munge_decode: %s", munge_strerror (e));
            errno = EKEYREJECTED;
            goto done;
        }
    }
    rc = 0;
done:
    return rc;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
