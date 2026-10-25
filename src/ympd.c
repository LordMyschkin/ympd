/* ympd
   (c) 2013-2014 Andrew Karpow <andy@ndyk.de>
   This project's homepage is: http://www.ympd.org

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include <getopt.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "config.h"
#include "ashuffle_control.h"
#include "http_server.h"
#include "mongoose.h"
#include "mpd_client.h"

extern char *optarg;

int force_exit = 0;

void bye() {
    force_exit = 1;
}

static int server_callback(struct mg_connection *c, enum mg_event ev) {
    int result = MG_FALSE;
    FILE *fp = NULL;

    switch (ev) {
        case MG_CLOSE:
            mpd_close_handler(c);
            return MG_TRUE;
        case MG_REQUEST:
            if (c->is_websocket) {
                c->content[c->content_len] = '\0';
                if (c->content_len)
                    return callback_mpd(c);
                else
                    return MG_TRUE;
            } else
#ifdef WITH_DYNAMIC_ASSETS
                if (!strcmp(c->uri, "/wss-auth")) {
                    unsigned char random_bytes[WSS_AUTH_TOKEN_SIZE];
                    char token[WSS_AUTH_TOKEN_SIZE + 1];
                    FILE *random = fopen("/dev/urandom", "rb");
                    if (!random) return MG_FALSE;
                    size_t got = fread(random_bytes, 1, sizeof(random_bytes), random);
                    fclose(random);
                    if (got != sizeof(random_bytes)) return MG_FALSE;
                    for (size_t i = 0; i < sizeof(random_bytes); ++i)
                        token[i] = 'A' + random_bytes[i] % 26;
                    token[sizeof(random_bytes)] = 0;
                    free(mpd.wss_auth_token);
                    mpd.wss_auth_token = strdup(token);
                    mg_send_header(c, "Content-Type", "text/plain");
                    mg_send_data(c, token, WSS_AUTH_TOKEN_SIZE);
                    return MG_TRUE;
                }
                return MG_FALSE;
#else
                return callback_http(c);
#endif
        case MG_AUTH:
            // no auth for websockets since mobile safari does not support it
            if ((mpd.gpass == NULL) || (c->is_websocket) ||
                ((mpd.local_port > 0) && (c->local_port == mpd.local_port)))
                return MG_TRUE;
            else {
                if ((fp = fopen(mpd.gpass, "r")) != NULL) {
                    result = mg_authorize_digest(c, fp);
                    fclose(fp);
                }
            }
            return result;
        default:
            return MG_FALSE;
    }
}

int main(int argc, char **argv) {
    int n, option_index = 0;
    struct mg_server *server = mg_create_server(NULL, server_callback);
    unsigned int current_timer = 0, last_timer = 0;
    char *run_as_user = NULL;
    char const *error_msg = NULL;
    char *webport = "6080";

    atexit(bye);
#ifdef WITH_DYNAMIC_ASSETS
    mg_set_option(server, "document_root", SRC_PATH);
#endif

    mg_set_option(server, "auth_domain", "ympd");
    mpd.port = 6600;
    mpd.local_port = 0;
    mpd.gpass = NULL;
    strcpy(mpd.host, "127.0.0.1");

    /* clang-format off */
    static struct option long_options[] = {
        {"digest",       required_argument, 0, 'D'},
        {"host",         required_argument, 0, 'h'},
        {"port",         required_argument, 0, 'p'},
        {"localport",    required_argument, 0, 'l'},
        {"webport",      required_argument, 0, 'w'},
        {"user",         required_argument, 0, 'u'},
        {"version",      no_argument,       0, 'v'},
        {"help",         no_argument,       0,  0 },
        {"mpdpass",      required_argument, 0, 'm'},
        {0,              0,                 0,  0 }
    };
    /* clang-format on */

    while ((n = getopt_long(argc, argv, "D:h:p:l:w:u:d:v:m", long_options, &option_index)) != -1) {
        switch (n) {
            case 'D':
                mpd.gpass = strdup(optarg);
                break;
            case 'h':
                strncpy(mpd.host, optarg, sizeof(mpd.host));
                break;
            case 'p':
                mpd.port = atoi(optarg);
                break;
            case 'l':
                mpd.local_port = atoi(optarg);
                break;
            case 'w':
                webport = strdup(optarg);
                break;
            case 'u':
                run_as_user = strdup(optarg);
                break;
            case 'm':
                if (strlen(optarg) > 0)
                    mpd.password = strdup(optarg);
                break;
            case 'v':
                fprintf(stdout,
                        "ympd  %d.%d.%d\n"
                        "Copyright (C) 2014 Andrew Karpow <andy@ndyk.de>\n"
                        "built " __DATE__
                        " "__TIME__
                        " ("__VERSION__
                        ")\n",
                        YMPD_VERSION_MAJOR, YMPD_VERSION_MINOR, YMPD_VERSION_PATCH);
                return EXIT_SUCCESS;
                break;
            default:
                fprintf(stderr,
                        "Usage: %s [OPTION]...\n\n"
                        " -D, --digest <htdigest>\tpath to htdigest file for authorization\n"
                        "                        \t(realm ympd) [no authorization]\n"
                        " -h, --host <host>\t\tconnect to mpd at host [localhost]\n"
                        " -p, --port <port>\t\tconnect to mpd at port [6600]\n"
                        " -l, --localport <port>\t\tskip authorization for local port\n"
                        " -w, --webport [ip:]<port>\tlisten interface/port for webserver [6080]\n"
                        " -u, --user <username>\t\tdrop priviliges to user after socket bind\n"
                        " -v, --version\t\t\tget version\n"
                        " -m, --mpdpass <password>\tspecifies the password to use when connecting "
                        "to mpd\n"
                        " --help\t\t\t\tthis help\n",
                        argv[0]);
                return EXIT_FAILURE;
        }

        if (error_msg) {
            fprintf(stderr, "Mongoose error: %s\n", error_msg);
            return EXIT_FAILURE;
        }
    }

    error_msg = mg_set_option(server, "listening_port", webport);
    if (error_msg) {
        fprintf(stderr, "Mongoose error: %s\n", error_msg);
        return EXIT_FAILURE;
    }

    /* drop privilges at last to ensure proper port binding */
    if (run_as_user != NULL) {
        error_msg = mg_set_option(server, "run_as_user", run_as_user);
        free(run_as_user);
        if (error_msg) {
            fprintf(stderr, "Mongoose error: %s\n", error_msg);
            return EXIT_FAILURE;
        }
    }

    ashuffle_init(argv[0]);
    while (!force_exit) {
        mg_poll_server(server, 200);
        current_timer = time(NULL);
        if (current_timer - last_timer) {
            last_timer = current_timer;
            mpd_poll(server);
            ashuffle_tick();
        }
    }

    ashuffle_shutdown();
    mpd_disconnect();
    mg_destroy_server(&server);

    return EXIT_SUCCESS;
}
