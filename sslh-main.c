/*
# main: processing of config file, command line options and start the main
# loop.
#
# Copyright (C) 2007-2018  Yves Rutschle
# 
# This program is free software; you can redistribute it
# and/or modify it under the terms of the GNU General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later
# version.
# 
# This program is distributed in the hope that it will be
# useful, but WITHOUT ANY WARRANTY; without even the implied
# warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
# PURPOSE.  See the GNU General Public License for more
# details.
# 
# The full text for the General Public License is here:
# http://www.gnu.org/licenses/gpl.html

*/

#define _GNU_SOURCE
#ifdef LIBCONFIG
#include <libconfig.h>
#endif
#ifdef ENABLE_REGEX
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#endif

#ifdef LIBBSD
#include <bsd/unistd.h>
#endif

#include "common.h"
#include "probe.h"
#include "log.h"

/* Constants for options that have no one-character shorthand */
#define OPT_ONTIMEOUT   257

static void printcaps(void) {
#ifdef LIBCAP
    cap_t caps;
    char* desc;
    ssize_t len;

    caps = cap_get_proc();

    desc = cap_to_text(caps, &len);

    print_message(msg_config, "capabilities: %s\n", desc);

    cap_free(caps);
    cap_free(desc);
#endif
}

static void printsettings(void)
{
    char buf[NI_MAXHOST];
    int i;
    struct sslhcfg_protocols_item *p;
    
    for (i = 0; i < cfg.protocols_len; i++ ) {
        p = &cfg.protocols[i];
        print_message(msg_config, 
                "%s addr: %s. libwrap service: %s log_level: %d family %d %d [%s] [%s] [%s]\n",
                p->name, 
                sprintaddr(buf, sizeof(buf), p->saddr), 
                p->service,
                p->log_level,
                p->saddr->ai_family,
                p->saddr->ai_addr->sa_family,
                p->keepalive ? "keepalive" : "",
                p->fork ? "fork" : "",
                p->transparent ? "transparent" : ""
                );
    }
    print_message(msg_config, "timeout: %d\non-timeout: %s\n", cfg.timeout,
            timeout_protocol()->name);
}


static void setup_regex_probe(struct sslhcfg_protocols_item *p)
#ifdef ENABLE_REGEX
{
    int num_patterns, i, error;
    pcre2_code** pattern_list;
    PCRE2_SIZE error_offset;
    PCRE2_UCHAR8 err_str[120];

    num_patterns = p->regex_patterns_len;

    pattern_list = calloc(num_patterns + 1, sizeof(*pattern_list));
    CHECK_ALLOC(pattern_list, "calloc");
    p->data = (void*)pattern_list;

    for (i = 0; i < num_patterns; i++) {
        pattern_list[i] = pcre2_compile((PCRE2_SPTR8)p->regex_patterns[i], 
                                        PCRE2_ZERO_TERMINATED, 0,
                                        &error, &error_offset, NULL);
        if (!pattern_list[i]) {
            pcre2_get_error_message(error, err_str, sizeof(err_str));
            print_message(msg_config_error, "compiling pattern /%s/:%d:%s at offset %ld\n",
                    p->regex_patterns[i], error, err_str, error_offset);
            exit(1);
        }
    }
}
#else
{
    return;
}
#endif

/* For each protocol in the configuration, resolve address and set up protocol
 * options if required
 */
static void config_protocols()
{
    int i;
    for (i = 0; i < cfg.protocols_len; i++) {
        struct sslhcfg_protocols_item* p = &(cfg.protocols[i]);

        if (
            !cfg.protocols.resolve_on_forward &&
            resolve_split_name(&(p->saddr), p->host, p->port)
        ) {
            print_message(msg_config_error, "cannot resolve %s:%s\n",
                          p->host, p->port);
            exit(4);
        }

        p->probe = get_probe(p->name);
        if (!p->probe) {
            print_message(msg_config_error, "%s: probe unknown\n", p->name);
            exit(1);
        }

        if (!strcmp(cfg.protocols[i].name, "regex")) {
            setup_regex_probe(&cfg.protocols[i]);
        }

        if (!strcmp(cfg.protocols[i].name, "tls")) {
            cfg.protocols[i].data = (void*)new_tls_data();
            if (cfg.protocols[i].sni_hostnames_len)
                tls_data_set_list(cfg.protocols[i].data, 0,
                                  (const char**) cfg.protocols[i].sni_hostnames,
                                  cfg.protocols[i].sni_hostnames_len);
            if (cfg.protocols[i].alpn_protocols_len)
                tls_data_set_list(cfg.protocols[i].data, 1, 
                                  (const char**) cfg.protocols[i].alpn_protocols,
                                  cfg.protocols[i].alpn_protocols_len);
        }
    }
}


void config_sanity_check(struct sslhcfg_item* cfg) {
    if (!cfg->protocols_len) {
        print_message(msg_config_error, "At least one target protocol must be specified.\n");
        exit(2);
    }

/* If compiling with systemd socket support no need to require listen address */
#ifndef SYSTEMD
    if (!cfg->listen_len && !cfg->inetd) {
        print_message(msg_config_error, "No listening address specified; use at least one -p option\n");
        exit(1);
    }
#endif
}


int main(int argc, char *argv[], char* envp[])
{

   extern char *optarg;
   extern int optind;
   int res, num_addr_listen;
   struct listen_endpoint *listen_sockets;

#ifdef LIBBSD
   setproctitle_init(argc, argv, envp);
#endif

   memset(&cfg, 0, sizeof(cfg));
   res = sslhcfg_cl_parse(argc, argv, &cfg);
   if (res) exit(6);

   if (cfg.version) {
       printf("%s %s\n", server_type, VERSION);
       exit(0);
   }

   config_protocols();
   config_sanity_check(&cfg);

   if (cfg.inetd)
   {
       close(fileno(stderr)); /* Make sure no error will go to client */
       start_shoveler(0);
       exit(0);
   }

   printsettings();

   num_addr_listen = start_listen_sockets(&listen_sockets);

#ifdef SYSTEMD
   if (num_addr_listen < 1) {
     print_message(msg_config_error, "No listening sockets found, restart sockets or specify addresses in config\n");
     exit(1);
    }
#endif

   if (!cfg.foreground) {
       if (fork() > 0) exit(0); /* Detach */

       /* New session -- become group leader */
       if (getuid() == 0) {
           res = setsid();
           CHECK_RES_DIE(res, "setsid: already process leader");
       }
   }

   setup_signals();

   if (cfg.pidfile)
       write_pid_file(cfg.pidfile);

   /* Open syslog connection before we drop privs/chroot */
   setup_syslog(argv[0]);

   if (cfg.user || cfg.chroot)
       drop_privileges(cfg.user, cfg.chroot);

   printcaps();

   print_message(msg_config, "%s %s started\n", server_type, VERSION);

   main_loop(listen_sockets, num_addr_listen);

   return 0;
}
