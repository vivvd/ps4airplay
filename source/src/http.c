/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lab.h"
#include "video_profile.h"
#include "web_page.h"
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define CLIENTS 4
#define REQUEST_CAP 4096
#define RESPONSE_CAP 45000
static int listener = -1;
static struct Client {
    int fd;
    char request[REQUEST_CAP], response[RESPONSE_CAP];
    size_t received, length, sent;
    time_t deadline;
} clients[CLIENTS];

static void drop(struct Client *c) { if (c->fd >= 0) close(c->fd); c->fd = -1; }
void lab_server_stop(void) {
    if (listener >= 0) close(listener);
    listener = -1;
    for (int i = 0; i < CLIENTS; ++i) drop(&clients[i]);
}
static int nonblock(int fd) {
#ifdef LAB_PS4
    /* Orbis sockets use SO_NBIO; fcntl on a socket can return EACCES. */
    const int on = 1;
    return setsockopt(fd, SOL_SOCKET, 0x1200, &on, sizeof(on));
#else
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}
int lab_server_start(LabState *s, const char *address, unsigned port) {
    for (int i = 0; i < CLIENTS; ++i) clients[i].fd = -1;
    if (port == 0 || port > 65535) return -1;
    snprintf(s->network_stage, sizeof(s->network_stage), "socket");
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) { s->net_rc = -errno; return -1; }
    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
#if defined(__FreeBSD__) || defined(__APPLE__)
    addr.sin_len = sizeof(addr);
#endif
    const char *stages[] = {"address", "bind", "listen", "nonblock"};
    for (int step = 0; step < 4; ++step) {
        snprintf(s->network_stage, sizeof(s->network_stage), "%s", stages[step]);
        int rc = step == 0 ? (inet_pton(AF_INET, address, &addr.sin_addr) == 1 ? 0 : -1) :
                 step == 1 ? bind(listener, (struct sockaddr *)&addr, sizeof(addr)) :
                 step == 2 ? listen(listener, CLIENTS) : nonblock(listener);
        if (rc < 0) {
            s->net_rc = errno ? -errno : -1;
            lab_log(s, "HTTP failure at %s: rc=%d errno=%d", stages[step], rc, errno);
            lab_server_stop(); return -1;
        }
    }
    snprintf(s->network_stage, sizeof(s->network_stage), "LISTENING");
    s->net_rc = 0;
    lab_log(s, "HTTP listening on %s:%u", address, port);
    return 0;
}
static void respond(struct Client *c, int code, const char *type, const char *body, int attachment) {
    const char *status = code == 200 ? "OK" : code == 202 ? "Accepted" : code == 403 ? "Forbidden" :
                         code == 404 ? "Not Found" : code == 405 ? "Method Not Allowed" : "Bad Request";
    size_t n = strlen(body);
    int h = snprintf(c->response, sizeof(c->response),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Connection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
        "%s\r\n", code, status, type, n,
        attachment ? "Content-Disposition: attachment; filename=ps4-receiver-report.json\r\n" : "");
    if (h < 0 || (size_t)h + n > sizeof(c->response)) { drop(c); return; }
    memcpy(c->response + h, body, n);
    c->length = (size_t)h + n; c->sent = 0;
}
/* Return a trimmed header value. Reject duplicate headers to avoid ambiguity. */
static int header(const char *request, const char *name, char *out, size_t cap) {
    const char *p = strstr(request, "\r\n");
    size_t len = strlen(name); int found = 0;
    out[0] = 0;
    if (!p) return 0;
    p += 2;
    while (*p && strncmp(p, "\r\n", 2)) {
        const char *end = strstr(p, "\r\n");
        if (!end) return -1;
        if ((size_t)(end - p) > len && !strncasecmp(p, name, len) && p[len] == ':') {
            if (found) return -1;
            const char *v = p + len + 1;
            while (v < end && (*v == ' ' || *v == '\t')) ++v;
            const char *last = end;
            while (last > v && (last[-1] == ' ' || last[-1] == '\t')) --last;
            size_t n = (size_t)(last - v);
            if (n >= cap) return -1;
            memcpy(out, v, n); out[n] = 0; found = 1;
        }
        p = end + 2;
    }
    return found;
}
static void route(LabState *s, struct Client *c) {
    char method[12], path[256], protocol[16], extra[2];
    char host[160], origin[200], expected[180], length[32], transfer[32];
    const char *line_end = strstr(c->request, "\r\n");
    if (!line_end || (size_t)(line_end - c->request) >= 300) goto bad;
    char line[300]; memcpy(line, c->request, line_end - c->request); line[line_end - c->request] = 0;
    if (sscanf(line, "%11s %255s %15s %1s", method, path, protocol, extra) != 3 ||
        strcmp(protocol, "HTTP/1.1") || path[0] != '/') goto bad;
    if (header(c->request, "Host", host, sizeof(host)) != 1 || !host[0] ||
        header(c->request, "Origin", origin, sizeof(origin)) < 0 ||
        header(c->request, "Content-Length", length, sizeof(length)) < 0 ||
        header(c->request, "Transfer-Encoding", transfer, sizeof(transfer)) != 0) goto bad;
    if (length[0] && strcmp(length, "0")) goto bad;
    ++s->requests;
    if (!strcmp(method, "GET")) {
        if (!strcmp(path, "/")) respond(c, 200, "text/html; charset=utf-8", lab_web_page, 0);
        else if (!strcmp(path, "/api/status") || !strcmp(path, "/api/report")) {
            char report[42000]; lab_report(s, report, sizeof(report));
            respond(c, 200, "application/json; charset=utf-8", report, !strcmp(path, "/api/report"));
        } else respond(c, 404, "text/plain", "Not found\n", 0);
    } else if (!strcmp(method, "POST")) {
        snprintf(expected, sizeof(expected), "http://%s", host);
        if (origin[0] && strcmp(origin, expected)) {
            respond(c, 403, "text/plain", "Cross-origin commands are disabled\n", 0); return;
        }
        if (!strncmp(path,"/api/video-profile/",19)) {
            int mode=lab_video_profile_id(path+19);
            if(mode<0){respond(c,400,"text/plain","Unknown video profile\n",0);return;}
            if(lab_video_profile_save(s,mode)!=0){respond(c,500,"text/plain","Cannot save video profile\n",0);return;}
        } else if (!strcmp(path, "/api/airplay-step")) {
            s->pending_airplay_step=1;lab_log(s,"AirPlay startup step requested");
        } else if (!strcmp(path, "/api/tone")) {
            s->pending_tone = 1; lab_log(s, "Tone requested from browser");
        } else if (!strcmp(path, "/api/pattern")) {
            s->pattern = 1; lab_log(s, "Framebuffer test pattern enabled");
        } else if (!strcmp(path, "/api/reset")) {
            s->pattern = 0; lab_log(s, "Status screen enabled");
        } else { respond(c, 404, "text/plain", "Not found\n", 0); return; }
        respond(c, 202, "application/json", "{\"accepted\":true}\n", 0);
    } else respond(c, 405, "text/plain", "Use GET for reports or POST for commands\n", 0);
    return;
bad:
    respond(c, 400, "text/plain", "Bad request\n", 0);
}
void lab_server_poll(LabState *s) {
    if (listener < 0) return;
    /* No blocking network calls in the video/input loop. */
    for (int i = 0; i < CLIENTS; ++i) {
        struct Client *c = &clients[i];
        if (c->fd < 0) {
            int fd = accept(listener, NULL, NULL);
            if (fd < 0) break;
            ++s->connections;
            if (nonblock(fd) < 0) { lab_log(s, "Client nonblock failed: %d", errno); close(fd); continue; }
            int yes = 1;
#ifdef SO_NOSIGPIPE
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#else
            (void)yes;
#endif
            memset(c, 0, sizeof(*c)); c->fd = fd; c->deadline = time(NULL) + 4;
        }
    }
    for (int i = 0; i < CLIENTS; ++i) {
        struct Client *c = &clients[i];
        if (c->fd < 0) continue;
        if (time(NULL) > c->deadline) { drop(c); continue; }
        if (!c->length) {
            ssize_t n = recv(c->fd, c->request + c->received, sizeof(c->request) - c->received - 1, 0);
            if (n == 0) { drop(c); continue; }
            if (n < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) drop(c);
                continue;
            }
            /* HTTP headers cannot contain embedded NUL. */
            if (memchr(c->request + c->received, 0, (size_t)n)) {
                respond(c, 400, "text/plain", "Bad request\n", 0);
            } else {
                c->received += (size_t)n; c->request[c->received] = 0;
                if (strstr(c->request, "\r\n\r\n")) route(s, c);
                else if (c->received == sizeof(c->request) - 1) respond(c, 400, "text/plain", "Headers too large\n", 0);
            }
        }
        if (c->length && c->fd >= 0) {
#ifdef MSG_NOSIGNAL
            int flags = MSG_NOSIGNAL;
#else
            int flags = 0;
#endif
            ssize_t n = send(c->fd, c->response + c->sent, c->length - c->sent, flags);
            if (n > 0) c->sent += (size_t)n;
            else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) { drop(c); continue; }
            if (c->sent == c->length) drop(c);
        }
    }
}
