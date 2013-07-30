#include "mheads.h"
#include "lheads.h"

static void tcp_recv(int fd, short event, void *arg);
static void process_buf(struct tcp_socket *tcpsock,
        unsigned char *buf, size_t len);

static void tcp_reply_mini(const struct req_info *req, uint32_t reply);
static void tcp_reply_err(const struct req_info *req, uint32_t reply);
static void tcp_reply_long(const struct req_info *req, uint32_t reply,
        unsigned char *val, size_t vsize);


/*
 * Miscelaneous helper functions
 */

void tcp_socket_free(struct tcp_socket *tcpsock)
{
    if (!tcpsock) return;
    
    //mtc_dbg("destroy tcpsock %d", tcpsock->fd);
    
    if (tcpsock->fd > 0) {
        close(tcpsock->fd);
        tcpsock->fd = -1;
    }
    if (tcpsock->evt) {
        event_del(tcpsock->evt);
        free(tcpsock->evt);
        tcpsock->evt = NULL;
    }
    if (tcpsock->buf)
        free(tcpsock->buf);
    if (tcpsock->on_close) {
        tcpsock->on_close(tcpsock->appdata);
        /*
         * avoid core dump
         */
        tcpsock->on_close = NULL;
        tcpsock->appdata = NULL;
    }
    free(tcpsock);
}

static void init_req(struct tcp_socket *tcpsock)
{
    tcpsock->req.fd = tcpsock->fd;
    tcpsock->req.type = REQTYPE_TCP;
    tcpsock->req.clisa = (struct sockaddr *) &tcpsock->clisa;
    tcpsock->req.clilen = tcpsock->clilen;
    tcpsock->req.reply_mini = tcp_reply_mini;
    tcpsock->req.reply_err = tcp_reply_err;
    tcpsock->req.reply_long = tcp_reply_long;

    tcpsock->req.tcpsock = tcpsock;
}

static void rep_send_error(const struct req_info *req, const unsigned int code)
{
    uint32_t l, r, c;
    unsigned char minibuf[4 * 4];

    /* Network format: length (4), ID (4), REP_ERR (4), error code (4) */
    l = htonl(4 + 4 + 4 + 4);
    r = htonl(REP_ERR);
    c = htonl(code);
    memcpy(minibuf, &l, 4);
    memcpy(minibuf + 4, &(req->id), 4);
    memcpy(minibuf + 8, &r, 4);
    memcpy(minibuf + 12, &c, 4);

    MSG_DUMP("send: ",  minibuf, 4 * 4);
    
    /* If this send fails, there's nothing to be done */
    r = send(req->fd, minibuf, 4 * 4, MSG_NOSIGNAL);

    if (r < 0) {
        mtc_err("rep_send_error() failed");
    }
}


static void tcp_reply_mini(const struct req_info *req, uint32_t reply);
static int rep_send(const struct req_info *req, const unsigned char *buf,
        const size_t size)
{
    ssize_t rv, c;

    MSG_DUMP("send: ",  buf, size);
    
    c = 0;
    while (c < size) {
        rv = send(req->fd, buf + c, size - c, MSG_NOSIGNAL);

        if (rv == size) {
            return 1;
        } else if (rv < 0) {
            if (errno != EAGAIN || errno != EWOULDBLOCK) {
                rep_send_error(req, REP_ERR_SEND);
                /* tcp_reply_mini will call rep_send(), cause core dump */
                //tcp_reply_mini(req, REP_ERR_SEND);
                return 0;
            } else {
                /* With big packets, the receiver window might
                 * get exhausted and send() would block, but
                 * as the fd is set in non-blocking mode, it
                 * returns EAGAIN. This makes us to retry when
                 * send() fails in this way.
                 *
                 * The proper way to fix this would be to add
                 * an event so we get notified when the fd is
                 * available for writing, and retry the send;
                 * but this is complex so leave it for when
                 * it's really needed. */
                continue;
            }
        } else if (rv == 0) {
            return 1;
        }

        c += rv;
    }

    return 1;
}


/* Send small replies, consisting in only a value. */
static void tcp_reply_mini(const struct req_info *req, uint32_t reply)
{
    /* We use a mini buffer to speedup the small replies, to avoid the
     * malloc() overhead. */
    uint32_t len;
    unsigned char minibuf[12];

    len = htonl(12);
    reply = htonl(reply);
    memcpy(minibuf, &len, 4);
    memcpy(minibuf + 4, &(req->id), 4);
    memcpy(minibuf + 8, &reply, 4);
    rep_send(req, minibuf, 12);
    return;
}


static void tcp_reply_err(const struct req_info *req, uint32_t reply)
{
    rep_send_error(req, reply);
}

static void tcp_reply_long(const struct req_info *req, uint32_t reply,
            unsigned char *val, size_t vsize)
{
    if (val == NULL) {
        /* miss */
        tcp_reply_mini(req, reply);
    } else {
        unsigned char *buf;
        size_t bsize;
        uint32_t t;

        reply = htonl(reply);

        /* The reply length is:
         * 4        total length
         * 4        id
         * 4        reply code
         * 4        vsize
         * vsize    val
         */
        bsize = 4 + 4 + 4 + 4 + vsize;
        buf = malloc(bsize);

        t = htonl(bsize);
        memcpy(buf, &t, 4);

        memcpy(buf + 4, &(req->id), 4);
        memcpy(buf + 8, &reply, 4);

        t = htonl(vsize);
        memcpy(buf + 12, &t, 4);
        memcpy(buf + 16, val, vsize);

        rep_send(req, buf, bsize);
        free(buf);
    }
    return;

}


/*
 * Main functions for receiving and parsing
 */

int tcp_init(const char *ip, int port)
{
    int fd, rv;
    struct sockaddr_in srvsa;
    struct in_addr ia;

    rv = inet_pton(AF_INET, ip, &ia);
    if (rv <= 0)
        return -1;

    srvsa.sin_family = AF_INET;
    srvsa.sin_addr.s_addr = ia.s_addr;
    srvsa.sin_port = htons(port);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    rv = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &rv, sizeof(rv)) < 0 ) {
        close(fd);
        return -1;
    }

    rv = bind(fd, (struct sockaddr *) &srvsa, sizeof(srvsa));
    if (rv < 0) {
        close(fd);
        return -1;
    }

    rv = listen(fd, 1024);
    if (rv < 0) {
        close(fd);
        return -1;
    }

    /* Disable nagle algorithm, as we often handle small amounts of data
     * it can make I/O quite slow. */
    rv = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &rv, sizeof(rv)) < 0 ) {
        close(fd);
        return -1;
    }

    return fd;
}


void tcp_close(int fd)
{
    close(fd);
}


/* Called by libevent for each receive event on our listen fd */
void tcp_newconnection(int fd, short event, void *arg)
{
    int newfd, optval;
    struct tcp_socket *tcpsock;
    struct event *new_event;

    tcpsock = malloc(sizeof(struct tcp_socket));
    if (tcpsock == NULL) {
        return;
    }
    tcpsock->clilen = sizeof(tcpsock->clisa);

    new_event = malloc(sizeof(struct event));
    if (new_event == NULL) {
        free(tcpsock);
        return;
    }

    newfd = accept(fd,
            (struct sockaddr *) &(tcpsock->clisa),
            &(tcpsock->clilen));

    if (fcntl(newfd, F_SETFL, O_NONBLOCK) != 0) {
        close(newfd);
        free(new_event);
        free(tcpsock);
        return;
    }

    optval = 1;
    setsockopt(newfd, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval));

    tcpsock->refcount = 0;
    tcpsock->fd = newfd;
    tcpsock->evt = new_event;
    tcpsock->buf = NULL;
    tcpsock->pktsize = 0;
    tcpsock->len = 0;
    tcpsock->excess = 0;
    tcpsock->appdata = NULL;
    tcpsock->on_close = NULL;

    tcp_socket_add_ref(tcpsock);

    event_set(new_event, newfd, EV_READ | EV_PERSIST, tcp_recv,
            (void *) tcpsock);
    event_add(new_event, NULL);

    return;
}


/* Static common buffer to avoid unnecessary allocation on the common case
 * where we get an entire single message on each recv().
 * Allocate a little bit more over the max. message size, which is 64kb. */
#define SBSIZE (68 * 1024)
static unsigned char static_buf[SBSIZE];

/* Called by libevent for each receive event */
static void tcp_recv(int fd, short event, void *arg)
{
    int rv;
    struct tcp_socket *tcpsock;

    tcpsock = (struct tcp_socket *) arg;

    if (tcpsock->buf == NULL) {
        /* New incoming message */
        rv = recv(fd, static_buf, SBSIZE, MSG_NOSIGNAL);
        if (rv < 0 && errno == EAGAIN) {
            /* We were awoken but have no data to read, so we do
             * nothing */
            return;
        } else if (rv == -1 && errno == ETIMEDOUT) {
            /*
             * network unreachable
             * may be reachable later, don't do anything
             */
            return;
        } else if (rv <= 0) {
            /* Orderly shutdown or error; close the file
             * descriptor in either case. */
            goto error_exit;
        }

        init_req(tcpsock);
        process_buf(tcpsock, static_buf, rv);
    } else {
        /* We already got a partial message, complete it. */
        size_t maxtoread = tcpsock->pktsize - tcpsock->len;

        rv = recv(fd, tcpsock->buf + tcpsock->len, maxtoread, MSG_NOSIGNAL);
        if (rv < 0 && errno == EAGAIN) {
            return;
        } else if (rv <= 0) {
            goto error_exit;
        }

        tcpsock->len += rv;

        process_buf(tcpsock, tcpsock->buf, tcpsock->len);
    }

    return;

error_exit:
    close(fd);
    tcpsock->fd = -1;
    /* avoid duplicate remove_ref() */
    event_del(tcpsock->evt);
    tcpsock->evt = NULL;
    tcp_socket_remove_ref(tcpsock);
    return;
}


/* Main message unwrapping */
static void process_buf(struct tcp_socket *tcpsock,
                        unsigned char *buf, size_t len)
{
    uint32_t totaltoget = 0;

    if (len >= 4) {
        totaltoget = * (uint32_t *) buf;
        totaltoget = ntohl(totaltoget);
        if (totaltoget > (64 * 1024) || totaltoget <= 8) {
            /* Message too big or too small, close the connection. */
            goto error_exit;
        }
    } else {
        /* If we didn't even read 4 bytes, we try to read 4 first and
         * then care about the rest. */
        totaltoget = 4;
    }

    if (totaltoget > len) {
        if (tcpsock->buf == NULL) {
            /* The first incomplete recv().
             * Create a temporary buffer and copy the contents of
             * our current one (which is static_buf, otherwise
             * tcpsock->buf wouldn't be NULL) to it. */
            tcpsock->buf = malloc(SBSIZE);
            if (tcpsock->buf == NULL)
                goto error_exit;

            memcpy(tcpsock->buf, buf, len);
            tcpsock->len = len;
            tcpsock->pktsize = totaltoget;
        } else {
            /* We already had an incomplete recv() and this is
             * just another one. */
            tcpsock->len = len;
            tcpsock->pktsize = totaltoget;
        }
        return;
    }

    if (totaltoget < len) {
        /* Got more than one message in the same recv(); save the
         * amount of bytes exceeding so we can process it later. */
        tcpsock->excess = len - totaltoget;
        len = totaltoget;
    }

    /* The buffer is complete, parse it as usual. */
    g_stat.msg_tcp++;
    if (parse_message(&(tcpsock->req), buf + 4, len - 4)) {
        goto exit;
    } else {
        goto error_exit;
    }


exit:
    if (tcpsock->excess) {
        /* If there are buffer leftovers (because there was more than
         * one message on a recv()), leave the buffer, move the
         * leftovers to the beginning, adjust the numbers and parse
         * recursively.
         * The buffer can be the static one or the one in tcpsock (if
         * we had a short recv()); we don't care because we know it
         * will be big enough to hold an entire message anyway. */
        memmove(buf, buf + len, tcpsock->excess);
        tcpsock->len = tcpsock->excess;
        tcpsock->excess = 0;

        /* Build a new req just like when we first recv(). */
        init_req(tcpsock);
        process_buf(tcpsock, buf, tcpsock->len);
        return;
    }

    if (tcpsock->buf) {
        /* We had an incomplete read somewhere along the processing of
         * this message, and had to malloc() a temporary space. free()
         * it and reset the associated information. */
        free(tcpsock->buf);
        tcpsock->buf = NULL;
        tcpsock->len = 0;
        tcpsock->pktsize = 0;
        tcpsock->excess = 0;
    }

    return;

error_exit:
    close(tcpsock->fd);
    tcpsock->fd = -1;
    /* avoid duplicate remove_ref() */
    event_del(tcpsock->evt);
    tcpsock->evt = NULL;
    tcp_socket_remove_ref(tcpsock);
    return;
}

void tcp_socket_add_ref(struct tcp_socket *tcpsock)
{
    if (!tcpsock) return;

    //mtc_dbg("add reference count on %d %d", tcpsock->fd, tcpsock->refcount);

    tcpsock->refcount++;
}

void tcp_socket_remove_ref(struct tcp_socket *tcpsock)
{
    if (!tcpsock || tcpsock->refcount <= 0) return;

    //mtc_dbg("remove reference count on %d %d", tcpsock->fd, tcpsock->refcount);

    tcpsock->refcount--;

    if (tcpsock->refcount == 0) tcp_socket_free(tcpsock);
}
