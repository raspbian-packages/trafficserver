/*
 * Copyright (c) 1985, 1989, 1993
 *    The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Portions Copyright (c) 1993 by Digital Equipment Corporation.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies, and that
 * the name of Digital Equipment Corporation not be used in advertising or
 * publicity pertaining to distribution of the document or software without
 * specific, written prior permission.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND DIGITAL EQUIPMENT CORP. DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS.   IN NO EVENT SHALL DIGITAL EQUIPMENT
 * CORPORATION BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
 * ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

/*
 * Copyright (c) 2004 by Internet Systems Consortium, Inc. ("ISC")
 * Portions Copyright (c) 1996-1999 by Internet Software Consortium.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
 * OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */


#if !defined (_WIN32)

#include "ink_platform.h"

#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <arpa/nameser.h>
#ifdef HAVE_ARPA_NAMESER_COMPAT_H
#include <arpa/nameser_compat.h>
#endif
#include <stdio.h>
#include <ctype.h>
#include <resolv.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "ink_string.h"
#include "ink_resolver.h"

#if !defined(linux)
int inet_aton(register const char *cp, struct in_addr *addr);
#endif

#if !defined(isascii)           /* XXX - could be a function */
# define isascii(c) (!(c & 0200))
#endif

#define DOT_SEPARATED(_x)                                   \
  ((unsigned char*)&(_x))[0], ((unsigned char*)&(_x))[1],   \
    ((unsigned char*)&(_x))[2], ((unsigned char*)&(_x))[3]

/*%
 * This routine is for closing the socket if a virtual circuit is used and
 * the program wants to close it.  This provides support for endhostent()
 * which expects to close the socket.
 *
 * This routine is not expected to be user visible.
 */
static void
ink_res_nclose(ink_res_state statp) {
  if (statp->_vcsock >= 0) {
    (void) close(statp->_vcsock);
    statp->_vcsock = -1;
    statp->_flags &= ~(INK_RES_F_VC | INK_RES_F_CONN);
  }
}

static void
ink_res_ndestroy(ink_res_state statp) {
  ink_res_nclose(statp);
  if (statp->_u._ext.ext != NULL)
    free(statp->_u._ext.ext);
  statp->options &= ~INK_RES_INIT;
  statp->_u._ext.ext = NULL;
}

static void
ink_res_setservers(ink_res_state statp, const union ink_res_sockaddr_union *set, int cnt) {
  int i, nserv;
  size_t size;

  /* close open servers */
  ink_res_nclose(statp);

  /* cause rtt times to be forgotten */
  statp->_u._ext.nscount = 0;

  nserv = 0;
  for (i = 0; i < cnt && nserv < INK_MAXNS; i++) {
    switch (set->sin.sin_family) {
      case AF_INET:
        size = sizeof(set->sin);
        if (statp->_u._ext.ext)
          memcpy(&statp->_u._ext.ext->nsaddrs[nserv], &set->sin, size);
        if (size <= sizeof(statp->nsaddr_list[nserv].sin))
          memcpy(&statp->nsaddr_list[nserv].sin, &set->sin, size);
        else
          statp->nsaddr_list[nserv].sin.sin_family = 0;
        nserv++;
        break;

#ifdef HAS_INET6_STRUCTS
      case AF_INET6:
        size = sizeof(set->sin6);
        if (statp->_u._ext.ext)
          memcpy(&statp->_u._ext.ext->nsaddrs[nserv], &set->sin6, size);
        if (size <= sizeof(statp->nsaddr_list[nserv].sin6))
          memcpy(&statp->nsaddr_list[nserv].sin6, &set->sin6, size);
        else
          statp->nsaddr_list[nserv].sin6.sin6_family = 0;
        nserv++;
        break;
#endif

      default:
        break;
    }
    set++;
  }
  statp->nscount = nserv;
}

int
ink_res_getservers(ink_res_state statp, union ink_res_sockaddr_union *set, int cnt) {
  int i;
  size_t size;
  u_int16_t family;

  for (i = 0; i < statp->nscount && i < cnt; i++) {
    if (statp->_u._ext.ext)
      family = statp->_u._ext.ext->nsaddrs[i].sin.sin_family;
    else
      family = statp->nsaddr_list[i].sin.sin_family;

    switch (family) {
      case AF_INET:
        size = sizeof(set->sin);
        if (statp->_u._ext.ext)
          memcpy(&set->sin, &statp->_u._ext.ext->nsaddrs[i], size);
        else
          memcpy(&set->sin, &statp->nsaddr_list[i].sin, size);
        break;

#ifdef HAS_INET6_STRUCTS
      case AF_INET6:
        size = sizeof(set->sin6);
        if (statp->_u._ext.ext)
          memcpy(&set->sin6, &statp->_u._ext.ext->nsaddrs[i], size);
        else
          memcpy(&set->sin6, &statp->nsaddr_list[i].sin6, size);
        break;
#endif

      default:
        set->sin.sin_family = 0;
        break;
    }
    set++;
  }
  return (statp->nscount);
}

static void
ink_res_setoptions(ink_res_state statp, const char *options, const char *source)
{
  NOWARN_UNUSED(source);
  const char *cp = options;
  int i;

#ifdef DEBUG
  if (statp->options & INK_RES_DEBUG)
    printf(";; res_setoptions(\"%s\", \"%s\")...\n", options, source);
#endif
  while (*cp) {
    /* skip leading and inner runs of spaces */
    while (*cp == ' ' || *cp == '\t')
      cp++;
    /* search for and process individual options */
    if (!strncmp(cp, "ndots:", sizeof("ndots:") - 1)) {
      i = atoi(cp + sizeof("ndots:") - 1);
      if (i <= INK_RES_MAXNDOTS)
        statp->ndots = i;
      else
        statp->ndots = INK_RES_MAXNDOTS;
#ifdef DEBUG
      if (statp->options & INK_RES_DEBUG)
        printf(";;\tndots=%d\n", statp->ndots);
#endif
    } else if (!strncmp(cp, "timeout:", sizeof("timeout:") - 1)) {
      i = atoi(cp + sizeof("timeout:") - 1);
      if (i <= INK_RES_MAXRETRANS)
        statp->retrans = i;
      else
        statp->retrans = INK_RES_MAXRETRANS;
#ifdef DEBUG
      if (statp->options & INK_RES_DEBUG)
        printf(";;\ttimeout=%d\n", statp->retrans);
#endif
#ifdef	SOLARIS2
    } else if (!strncmp(cp, "retrans:", sizeof("retrans:") - 1)) {
      /*
       * For backward compatibility, 'retrans' is
       * supported as an alias for 'timeout', though
       * without an imposed maximum.
       */
      statp->retrans = atoi(cp + sizeof("retrans:") - 1);
    } else if (!strncmp(cp, "retry:", sizeof("retry:") - 1)){
      /*
       * For backward compatibility, 'retry' is
       * supported as an alias for 'attempts', though
       * without an imposed maximum.
       */
      statp->retry = atoi(cp + sizeof("retry:") - 1);
#endif	/* SOLARIS2 */
    } else if (!strncmp(cp, "attempts:", sizeof("attempts:") - 1)){
      i = atoi(cp + sizeof("attempts:") - 1);
      if (i <= INK_RES_MAXRETRY)
        statp->retry = i;
      else
        statp->retry = INK_RES_MAXRETRY;
#ifdef DEBUG
      if (statp->options & INK_RES_DEBUG)
        printf(";;\tattempts=%d\n", statp->retry);
#endif
    } else if (!strncmp(cp, "debug", sizeof("debug") - 1)) {
#ifdef DEBUG
      if (!(statp->options & INK_RES_DEBUG)) {
        printf(";; res_setoptions(\"%s\", \"%s\")..\n", options, source);
        statp->options |= INK_RES_DEBUG;
      }
      printf(";;\tdebug\n");
#endif
    } else if (!strncmp(cp, "no_tld_query", sizeof("no_tld_query") - 1) ||
               !strncmp(cp, "no-tld-query", sizeof("no-tld-query") - 1)) {
      statp->options |= INK_RES_NOTLDQUERY;
    } else if (!strncmp(cp, "inet6", sizeof("inet6") - 1)) {
      statp->options |= INK_RES_USE_INET6;
    } else if (!strncmp(cp, "rotate", sizeof("rotate") - 1)) {
      statp->options |= INK_RES_ROTATE;
    } else if (!strncmp(cp, "no-check-names", sizeof("no-check-names") - 1)) {
      statp->options |= INK_RES_NOCHECKNAME;
    }
#ifdef INK_RES_USE_EDNS0
    else if (!strncmp(cp, "edns0", sizeof("edns0") - 1)) {
      statp->options |= INK_RES_USE_EDNS0;
    }
#endif
    else if (!strncmp(cp, "dname", sizeof("dname") - 1)) {
      statp->options |= INK_RES_USE_DNAME;
    }
    else {
      /* XXX - print a warning here? */
    }
    /* skip to next run of spaces */
    while (*cp && *cp != ' ' && *cp != '\t')
      cp++;
  }
}

static u_int
ink_res_randomid(void) {
  struct timeval now;

  gettimeofday(&now, NULL);
  return (0xffff & (now.tv_sec ^ now.tv_usec ^ getpid()));
}

/*%
 * Set up default settings.  If the configuration file exist, the values
 * there will have precedence.  Otherwise, the server address is set to
 * INADDR_ANY and the default domain name comes from the gethostname().
 *
 * An interrim version of this code (BIND 4.9, pre-4.4BSD) used 127.0.0.1
 * rather than INADDR_ANY ("0.0.0.0") as the default name server address
 * since it was noted that INADDR_ANY actually meant ``the first interface
 * you "ifconfig"'d at boot time'' and if this was a SLIP or PPP interface,
 * it had to be "up" in order for you to reach your own name server.  It
 * was later decided that since the recommended practice is to always
 * install local static routes through 127.0.0.1 for all your network
 * interfaces, that we could solve this problem without a code change.
 *
 * The configuration file should always be used, since it is the only way
 * to specify a default domain.  If you are running a server on your local
 * machine, you should say "nameserver 0.0.0.0" or "nameserver 127.0.0.1"
 * in the configuration file.
 *
 * Return 0 if completes successfully, -1 on error
 */
/*% This function has to be reachable by res_data.c but not publically. */
int
ink_res_init(ink_res_state statp, const unsigned int *pHostList, const int *pPort, const char *pDefDomain, const char *pSearchList,
             const char *pResolvConf) {
  register FILE *fp;
  register char *cp, **pp;
  register int n;
  char buf[BUFSIZ];
  int nserv = 0;
  int haveenv = 0;
  int havesearch = 0;
  int dots;
  int maxns = INK_MAXNS;

  // INK_RES_SET_H_ERRNO(statp, 0);
  statp->res_h_errno = 0;
  if (statp->_u._ext.ext != NULL)
    ink_res_ndestroy(statp);

  statp->retrans = INK_RES_TIMEOUT;
  statp->retry = INK_RES_DFLRETRY;
  statp->options = INK_RES_DEFAULT;
  statp->id = ink_res_randomid();

  statp->nscount = 0;
  statp->ndots = 1;
  statp->pfcode = 0;
  statp->_vcsock = -1;
  statp->_flags = 0;
  statp->qhook = NULL;
  statp->rhook = NULL;
  statp->_u._ext.nscount = 0;
  statp->_u._ext.ext = (struct __ink_res_state_ext*)malloc(sizeof(*statp->_u._ext.ext));
  if (statp->_u._ext.ext != NULL) {
    memset(statp->_u._ext.ext, 0, sizeof(*statp->_u._ext.ext));
    statp->_u._ext.ext->nsaddrs[0].sin = statp->nsaddr_list[0].sin;
  } else {
    /*
     * Historically res_init() rarely, if at all, failed.
     * Examples and applications exist which do not check
     * our return code.  Furthermore several applications
     * simply call us to get the systems domainname.  So
     * rather then immediately fail here we store the
     * failure, which is returned later, in h_errno.  And
     * prevent the collection of 'nameserver' information
     * by setting maxns to 0.  Thus applications that fail
     * to check our return code wont be able to make
     * queries anyhow.
     */
    // INK_RES_SET_H_ERRNO(statp, NETDB_INTERNAL);
    statp->res_h_errno = NETDB_INTERNAL;
    maxns = 0;
  }

#ifdef	SOLARIS2
  /*
   * The old libresolv derived the defaultdomain from NIS/NIS+.
   * We want to keep this behaviour
   */
  {
    char buf[sizeof(statp->defdname)], *cp;
    int ret;

    if ((ret = sysinfo(SI_SRPC_DOMAIN, buf, sizeof(buf))) > 0 &&
        (unsigned int)ret <= sizeof(buf)) {
      if (buf[0] == '+')
        buf[0] = '.';
      cp = strchr(buf, '.');
      cp = (cp == NULL) ? buf : (cp + 1);
      strncpy(statp->defdname, cp,
              sizeof(statp->defdname) - 1);
      statp->defdname[sizeof(statp->defdname) - 1] = '\0';
    }
  }
#endif	/* SOLARIS2 */

	/* Allow user to override the local domain definition */
  if ((cp = getenv("LOCALDOMAIN")) != NULL) {
    (void)strncpy(statp->defdname, cp, sizeof(statp->defdname) - 1);
    statp->defdname[sizeof(statp->defdname) - 1] = '\0';
    haveenv++;

    /*
     * Set search list to be blank-separated strings
     * from rest of env value.  Permits users of LOCALDOMAIN
     * to still have a search list, and anyone to set the
     * one that they want to use as an individual (even more
     * important now that the rfc1535 stuff restricts searches)
     */
    cp = statp->defdname;
    pp = statp->dnsrch;
    *pp++ = cp;
    for (n = 0; *cp && pp < statp->dnsrch + INK_MAXDNSRCH; cp++) {
      if (*cp == '\n')	/*%< silly backwards compat */
        break;
      else if (*cp == ' ' || *cp == '\t') {
        *cp = 0;
        n = 1;
      } else if (n) {
        *pp++ = cp;
        n = 0;
        havesearch = 1;
      }
    }
    /* null terminate last domain if there are excess */
    while (*cp != '\0' && *cp != ' ' && *cp != '\t' && *cp != '\n')
      cp++;
    *cp = '\0';
    *pp++ = 0;
  }

  /* ---------------------------------------------
     Default domain name and doamin Search list:

     if we are supplied a default domain name,
     and/or search list we will use it. Otherwise,
     we will skip to using  what is present in the
     conf file
     ---------------------------------------------- */

  if (pDefDomain && '\0' != *pDefDomain && '\n' != *pDefDomain) {
    strncpy(statp->defdname, pDefDomain, sizeof(statp->defdname) - 1);
    if ((cp = strpbrk(statp->defdname, " \t\n")) != NULL)
      *cp = '\0';
  }
  if (pSearchList && '\0' != *pSearchList && '\n' != *pSearchList) {
    strncpy(statp->defdname, pSearchList, sizeof(statp->defdname) - 1);
    if ((cp = strchr(statp->defdname, '\n')) != NULL)
      *cp = '\0';
    /*
     * Set search list to be blank-separated strings
     * on rest of line.
     */
    cp = statp->defdname;
    pp = statp->dnsrch;
    *pp++ = cp;
    for (n = 0; *cp && pp < statp->dnsrch + INK_MAXDNSRCH; cp++) {
      if (*cp == ' ' || *cp == '\t') {
        *cp = 0;
        n = 1;
      } else if (n) {
        *pp++ = cp;
        n = 0;
      }
    }
    /* null terminate last domain if there are excess */
    while (*cp != '\0' && *cp != ' ' && *cp != '\t')
      cp++;
    *cp = '\0';
    *pp++ = 0;
    havesearch = 1;
  }

  /* -------------------------------------------
     we must be provided with atleast a named!
     ------------------------------------------- */
  /* TODO we should figure out the IPV6 resolvers here. */
  while (pHostList && pHostList[nserv] != 0 && nserv < INK_MAXNS) {
    statp->nsaddr_list[nserv].sin.sin_addr.s_addr = pHostList[nserv];
    statp->nsaddr_list[nserv].sin.sin_family = AF_INET;
    statp->nsaddr_list[nserv].sin.sin_port = htons(pPort[nserv]);
    nserv++;
  }

#define	MATCH(line, name)                       \
  (!strncmp(line, name, sizeof(name) - 1) &&    \
   (line[sizeof(name) - 1] == ' ' ||            \
    line[sizeof(name) - 1] == '\t'))

  if ((fp = fopen(pResolvConf, "r")) != NULL) {
    /* read the config file */
    while (fgets(buf, sizeof(buf), fp) != NULL) {
      /* skip comments */
      if (*buf == ';' || *buf == '#')
        continue;
      /* read default domain name */
      if (MATCH(buf, "domain")) {
        if (haveenv)	/*%< skip if have from environ */
          continue;
        cp = buf + sizeof("domain") - 1;
        while (*cp == ' ' || *cp == '\t')
          cp++;
        if ((*cp == '\0') || (*cp == '\n'))
          continue;
        strncpy(statp->defdname, cp, sizeof(statp->defdname) - 1);
        statp->defdname[sizeof(statp->defdname) - 1] = '\0';
        if ((cp = strpbrk(statp->defdname, " \t\n")) != NULL)
          *cp = '\0';
        havesearch = 0;
        continue;
      }
      /* set search list */
      if (MATCH(buf, "search")) {
        if (haveenv)	/*%< skip if have from environ */
          continue;
        cp = buf + sizeof("search") - 1;
        while (*cp == ' ' || *cp == '\t')
          cp++;
        if ((*cp == '\0') || (*cp == '\n'))
          continue;
        strncpy(statp->defdname, cp, sizeof(statp->defdname) - 1);
        statp->defdname[sizeof(statp->defdname) - 1] = '\0';
        if ((cp = strchr(statp->defdname, '\n')) != NULL)
          *cp = '\0';
        /*
         * Set search list to be blank-separated strings
         * on rest of line.
         */
        cp = statp->defdname;
        pp = statp->dnsrch;
        *pp++ = cp;
        for (n = 0; *cp && pp < statp->dnsrch + INK_MAXDNSRCH; cp++) {
          if (*cp == ' ' || *cp == '\t') {
            *cp = 0;
            n = 1;
          } else if (n) {
            *pp++ = cp;
            n = 0;
          }
        }
        /* null terminate last domain if there are excess */
        while (*cp != '\0' && *cp != ' ' && *cp != '\t')
          cp++;
        *cp = '\0';
        *pp++ = 0;
        havesearch = 1;
        continue;
      }
      /* read nameservers to query */
      if (MATCH(buf, "nameserver") && nserv < maxns) {
        struct addrinfo hints, *ai;
        char sbuf[NI_MAXSERV];
        const size_t minsiz =
          sizeof(statp->_u._ext.ext->nsaddrs[0]);

        cp = buf + sizeof("nameserver") - 1;
        while (*cp == ' ' || *cp == '\t')
          cp++;
        cp[strcspn(cp, ";# \t\n")] = '\0';
        if ((*cp != '\0') && (*cp != '\n')) {
          memset(&hints, 0, sizeof(hints));
          hints.ai_family = PF_UNSPEC;
          hints.ai_socktype = SOCK_DGRAM;	/*dummy*/
          hints.ai_flags = AI_NUMERICHOST;
          sprintf(sbuf, "%d", NAMESERVER_PORT);
          if (getaddrinfo(cp, sbuf, &hints, &ai) == 0 &&
              ai->ai_addrlen <= minsiz) {
            if (statp->_u._ext.ext != NULL) {
              memcpy(&statp->_u._ext.ext->nsaddrs[nserv], ai->ai_addr, ai->ai_addrlen);
            }
            if (ai->ai_addrlen <= sizeof(statp->nsaddr_list[nserv].sin)) {
              memcpy(&statp->nsaddr_list[nserv].sin, ai->ai_addr, ai->ai_addrlen);
            } else
              statp->nsaddr_list[nserv].sin.sin_family = 0;
            freeaddrinfo(ai);
            nserv++;
          }
        }
        continue;
      }
      if (MATCH(buf, "options")) {
        ink_res_setoptions(statp, buf + sizeof("options") - 1, "conf");
        continue;
      }
    }
    (void) fclose(fp);
  }

  if (nserv > 0)
    statp->nscount = nserv;

  if (statp->defdname[0] == 0 && gethostname(buf, sizeof(statp->defdname) - 1) == 0 && (cp = strchr(buf, '.')) != NULL)
    strcpy(statp->defdname, cp + 1);

  /* find components of local domain that might be searched */
  if (havesearch == 0) {
    pp = statp->dnsrch;
    *pp++ = statp->defdname;
    *pp = NULL;

    dots = 0;
    for (cp = statp->defdname; *cp; cp++)
      dots += (*cp == '.');

    cp = statp->defdname;
    while (pp < statp->dnsrch + INK_MAXDFLSRCH) {
      if (dots < INK_LOCALDOMAINPARTS)
        break;
      cp = strchr(cp, '.') + 1;    /*%< we know there is one */
      *pp++ = cp;
      dots--;
    }
    *pp = NULL;
#ifdef DEBUG
    if (statp->options & INK_RES_DEBUG) {
      printf(";; res_init()... default dnsrch list:\n");
      for (pp = statp->dnsrch; *pp; pp++)
        printf(";;\t%s\n", *pp);
      printf(";;\t..END..\n");
    }
#endif
  }

  /* export all ns servers to DNSprocessor. */
  ink_res_setservers(statp, &statp->nsaddr_list[0], statp->nscount);

  if ((cp = getenv("RES_OPTIONS")) != NULL)
    ink_res_setoptions(statp, cp, "env");
  statp->options |= INK_RES_INIT;
  return (statp->res_h_errno);
}

#endif
