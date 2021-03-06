#include "moc_plugin.h"
#include "moc_base.h"
#include "base_pri.h"

NEOERR* base_info_init(struct base_info **binfo)
{
    NEOERR *err;

    MCS_NOT_NULLA(binfo);

    if (!*binfo) {
        struct base_info *linfo = calloc(1, sizeof(struct base_info));
        if (!linfo) return nerr_raise(NERR_NOMEM, "alloc base failure");
        linfo->usernum = 0;
        err = hash_init(&linfo->userh, hash_str_hash, hash_str_comp, NULL);
        if (err != STATUS_OK) return nerr_pass(err);

        *binfo = linfo;
    }

    return STATUS_OK;
}

void base_info_destroy(BaseInfo *binfo)
{
    char *key = NULL;
    
    if (!binfo) return;

    HASH *table = binfo->userh;
    BaseUser *user = hash_next(table, (void**)&key);
    while (user) {
        base_user_destroy(user);
        
        user = hash_next(table, (void**)&key);
    }

    hash_destroy(&binfo->userh);

    free(binfo);
}


/*
 * user
 */
struct base_user *base_user_find(struct base_info *binfo, char *uid)
{
    if (!binfo || !uid) return NULL;
    
    return (struct base_user*)hash_lookup(binfo->userh, uid);
}

struct base_user *base_user_new(struct base_info *binfo, char *uid, QueueEntry *q,
                                struct base_user *ruser, void (*user_destroy)(void *arg))
{
    if (!binfo || !uid || !q || !q->req) return NULL;
    
    struct base_user *user;

    user = base_user_find(binfo, uid);
    if (user) return user;
    
    if (ruser) user = ruser;
    else user = calloc(1, sizeof(struct base_user));

    if (!user) return NULL;

    struct sockaddr_in *clisa = (struct sockaddr_in*)q->req->clisa;

    //strncpy(user->ip, inet_ntoa(clisa), 16);
    user->uid = strdup(uid);
    user->fd = q->req->fd;
    inet_ntop(clisa->sin_family, &clisa->sin_addr,
              user->ip, sizeof(user->ip));
    user->port = ntohs(clisa->sin_port);
    user->tcpsock = q->req->tcpsock;
    user->baseinfo = binfo;

    /*
     * used on user close
     */
    if (q->req->tcpsock) {
        q->req->tcpsock->appdata = user;
        if (user_destroy)
            q->req->tcpsock->on_close = user_destroy;
        else
            q->req->tcpsock->on_close = base_user_destroy;
    }
    
    /*
     * binfo
     */
    hash_insert(binfo->userh, (void*)strdup(uid), (void*)user);
    binfo->usernum++;
    
    mtc_dbg("%s %s %d join", uid, user->ip, user->port);
    
    return user;
}

bool base_user_quit(struct base_info *binfo, char *uid,
                    QueueEntry *q, void (*user_destroy)(void *arg))
{
    struct base_user *user;

    user = base_user_find(binfo, uid);
    if (!user) return false;
    
    if (q && q->req->tcpsock) {
        if (q->req->tcpsock == user->tcpsock && q->req->fd == user->fd)
            return false;
    }
    
    mtc_dbg("%s %s %d quit", user->uid, user->ip, user->port);

    tcp_socket_remove_ref(user->tcpsock);
    /* user may be destroied by remove_ref(), so, don't write */
    //user->tcpsock = NULL;

    return true;
}

void base_user_destroy(void *arg)
{
    struct base_user *user = (struct base_user*)arg;
    struct base_info *binfo = user->baseinfo;

    if (!user || !binfo) return;
    
    mtc_dbg("%s %s %d destroy", user->uid, user->ip, user->port);

    hash_remove(binfo->userh, user->uid);
    if (binfo->usernum > 0) binfo->usernum--;

    SAFE_FREE(user->uid);
    SAFE_FREE(user);

    return;
}


/*
 * msg
 */
static unsigned char static_buf[MAX_PACKET_LEN];

NEOERR* base_msg_new(char *cmd, HDF *datanode, unsigned char **buf, size_t *size)
{
    NEOERR *err;

    MCS_NOT_NULLC(cmd, datanode, buf);
    MCS_NOT_NULLA(size);

    size_t bsize, vsize;
    unsigned char *rbuf;
    uint32_t t;

    memset(static_buf, 0x0, MAX_PACKET_LEN);

    hdf_set_value(datanode, "_Reserve", "moc");
    err = hdf_set_attr(datanode, "_Reserve", "cmd", cmd);
    if (err != STATUS_OK) return nerr_pass(err);

    TRACE_HDF(datanode);

    vsize = pack_hdf(datanode, static_buf, MAX_PACKET_LEN);
    if(vsize <= 0) return nerr_raise(NERR_ASSERT, "packet error");

    /*
     * copy from tcp.c tcp_reply_long()
     */
    bsize = 4 + 4 + 4 + 4 + vsize;
    rbuf = calloc(1, bsize);
    if (!rbuf) return nerr_raise(NERR_NOMEM, "alloc msg buffer");

    t = htonl(bsize);
    memcpy(rbuf, &t, 4);

    /*
     * server 主动发给 client 的包，reqid == 0, && reply == 10000
     */
    t = 0;
    memcpy(rbuf + 4, &t, 4);
    t = htonl(10000);
    memcpy(rbuf + 8, &t, 4);
    
    t = htonl(vsize);
    memcpy(rbuf + 12, &t, 4);
    memcpy(rbuf + 16, static_buf, vsize);

    *buf = rbuf;
    *size = bsize;

    return STATUS_OK;
}

NEOERR* base_msg_send(unsigned char *buf, size_t size, int fd)
{
    MCS_NOT_NULLA(buf);
    if (fd <= 0) return nerr_raise(NERR_ASSERT, "fd 非法");

    /*
     * copy from tcp.c rep_send()
     */
    size_t rv, c;

    MSG_DUMP("send: ",  buf, size);
    
    c = 0;
    while (c < size) {
        rv = send(fd, buf + c, size - c, 0);

        if (rv == size) return STATUS_OK;
        else if (rv == 0) return STATUS_OK;
        else if (rv < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return nerr_raise(NERR_IO, "send return %ld", rv);
        }

        c += rv;
    }

    return STATUS_OK;
}

void base_msg_free(unsigned char *buf)
{
    if (!buf) return;
    free(buf);
}

NEOERR* base_msg_touser(char *cmd, HDF *datanode, int fd)
{
    unsigned char *buf;
    size_t len;
    NEOERR *err;

    MCS_NOT_NULLB(cmd, datanode);

    err = base_msg_new(cmd, datanode, &buf, &len);
    if (err != STATUS_OK) return nerr_pass(err);

    err = base_msg_send(buf, len, fd);
    if (err != STATUS_OK) return nerr_pass(err);

    base_msg_free(buf);

    return STATUS_OK;
}
