#include "mheads.h"
#include "lheads.h"

static void parse_stats(struct queue_entry *q)
{
    hdf_set_int_value(q->hdfsnd, "msg_tipc", g_stat.msg_tipc);
    hdf_set_int_value(q->hdfsnd, "msg_tcp", g_stat.msg_tcp);
    hdf_set_int_value(q->hdfsnd, "msg_udp", g_stat.msg_udp);
    hdf_set_int_value(q->hdfsnd, "msg_sctp", g_stat.msg_sctp);
    hdf_set_int_value(q->hdfsnd, "net_version_mismatch", g_stat.net_version_mismatch);
    hdf_set_int_value(q->hdfsnd, "net_broken_req", g_stat.net_broken_req);
    hdf_set_int_value(q->hdfsnd, "net_unk_req", g_stat.net_unk_req);
    hdf_set_int_value(q->hdfsnd, "pro_busy", g_stat.pro_busy);

    reply_trigger(q, REP_OK);

    return;
}

static void parse_clientmod(struct queue_entry *q)
{
    HDF *node = hdf_get_obj(g_cfg, "Client.modules");
    //unsigned short cmd = q->req->cmd;
    
    if (node) {
        hdf_copy(q->hdfsnd, "modules", node);
        reply_trigger(q, REP_OK);
    } else q->req->reply_mini(q->req, REP_ERR_UNKREQ);
    
    return;
}


/* Create a queue entry structure based on the parameters passed. Memory
 * allocated here will be free()'d in queue_entry_free(). It's not the
 * cleanest way, but the alternatives are even messier. */
static struct queue_entry *make_queue_long_entry(const struct req_info *req,
                                                 const unsigned char *ename,
                                                 size_t esize,
                                                 HDF *hdfrcv)
{
    struct queue_entry *e;
    unsigned char *ecopy;

    e = queue_entry_create();
    if (e == NULL) {
        return NULL;
    }

    ecopy = NULL;
    if (ename != NULL) {
        ecopy = malloc(esize);
        if (ecopy == NULL) {
            queue_entry_free(e);
            return NULL;
        }
        memcpy(ecopy, ename, esize);
    }

    e->operation = (uint32_t)req->cmd;
    e->ename = ecopy;
    e->esize = esize;
    e->hdfrcv = hdfrcv;

    /* Create a copy of req, including clisa */
    e->req = malloc(sizeof(struct req_info));
    if (e->req == NULL) {
        queue_entry_free(e);
        return NULL;
    }
    memcpy(e->req, req, sizeof(struct req_info));

    e->req->clisa = malloc(req->clilen);
    if (e->req->clisa == NULL) {
        queue_entry_free(e);
        return NULL;
    }
    memcpy(e->req->clisa, req->clisa, req->clilen);

    /* clear out unused fields */
    e->req->payload = NULL;
    e->req->psize = 0;
    e->req->tcpsock = req->tcpsock;

    return e;
}


/* Creates a new queue entry and puts it into the queue. Returns 1 if success,
 * 0 if memory error. */
static int put_in_queue_long(const struct req_info *req, int sync,
                             const unsigned char *ename, size_t esize,
                             HDF *hdfrcv)
{
    struct queue_entry *e;

    struct event_entry *entry = find_entry_in_table(g_moc, ename, esize);
    if (entry == NULL) {
        if (!strncmp((char*)ename, "_Reserve.Status", esize)) {
            e = make_queue_long_entry(req, ename, esize, hdfrcv);
            if (e == NULL) {
                return 0;
            }
            parse_stats(e);
            queue_entry_free(e);
            return 1;
        } else if (!strncmp((char*)ename, "_Reserve.Clientmod", esize)) {
            e = make_queue_long_entry(req, ename, esize, hdfrcv);
            if (e == NULL) {
                return 0;
            }
            parse_clientmod(e);
            queue_entry_free(e);
            return 1;
        }
        hdf_destroy(&hdfrcv);
        g_stat.net_unk_req++;
        if (sync) req->reply_mini(req, REP_ERR_UNKREQ);
        return 1;
    }
    
    if (entry->op_queue->size > QUEUE_SIZE_WARNING &&
        entry->op_queue->size % 100 == 0) {
        mtc_err("plugin %s size exceed %ld",
                entry->name, entry->op_queue->size);
    }
    if (entry->op_queue->size > MAX_QUEUE_ENTRY &&
        entry->op_queue->size % 100 == 0) {
        mtc_foo("plugin %s busy, queue size is %ld",
                entry->name, entry->op_queue->size);
        hdf_destroy(&hdfrcv);
        g_stat.pro_busy++;
        if (sync) req->reply_mini(req, REP_ERR_BUSY);
        return 1;
    }
    
    e = make_queue_long_entry(req, ename, esize, hdfrcv);
    if (e == NULL) {
        return 0;
    }

    queue_lock(entry->op_queue);
    if (sync) queue_cas(entry->op_queue, e);
    else queue_put(entry->op_queue, e);
    queue_unlock(entry->op_queue);

#if 0
    if (sync) {
        /* Signal the app thread it has work only if it's a
         * synchronous operation, asynchronous don't mind
         * waiting. It does have a measurable impact on
         * performance (2083847usec vs 2804973usec for sets on
         * "test2d 100000 10 10"). */
        queue_signal(entry->op_queue);
    }
#endif

    /*
     * TEMP
     * Always signal the app thread
     * Because current moc client SYNC mode will block the main thread
     * Caller always used the ASYNC mode and expect response as soon as possible
     */
    queue_signal(entry->op_queue);
    
    return 1;
}

/* Like put_in_queue_long() but with few parameters because most actions do
 * not need newval. */
static int put_in_queue(const struct req_info *req, int sync,
                        const unsigned char *ename, size_t esize,
                        HDF *hdfrcv)
{
    return put_in_queue_long(req, sync, ename, esize, hdfrcv);
}


#define FILL_SYNC_FLAG()                        \
    do {                                        \
        sync = req->flags & FLAGS_SYNC;         \
    } while(0)

static void parse_event(struct req_info *req)
{
    int rv, sync;
    const unsigned char *ename;
    uint32_t esize, rsize;
    unsigned char *pos;
    HDF *hdfrcv = NULL;

    FILL_SYNC_FLAG();
    
    /*
     * Request format:
     * 4        esize
     * esize    ename
     *
     * 4        vtype
     * 4        ksize
     * ksize    key
     * 4        vsize/ival
     * vsize    val
     *
     */
    pos = (unsigned char*)req->payload;
    esize = * (uint32_t *) pos; esize = ntohl(esize);

    pos = pos + sizeof(uint32_t);
    ename = pos;

    pos = pos + esize;
    rsize = unpack_hdf(pos, req->psize-esize-sizeof(uint32_t), &hdfrcv);
    if (rsize == 0 || rsize+esize+sizeof(uint32_t) > MAX_PACKET_LEN ||
        req->psize < esize) {
        g_stat.net_broken_req++;
        if (sync) req->reply_mini(req, REP_ERR_BROKEN);
        return;
    }

    rv = put_in_queue(req, sync, ename, esize, hdfrcv);
    if (!rv) {
        if (sync) req->reply_mini(req, REP_ERR_MEM);
        return;
    }

    return;
}


/* Parse an incoming message. Note that the protocol might have sent this
 * directly over the network (ie. TIPC) or might have wrapped it around (ie.
 * TCP). Here we only deal with the clean, stripped, non protocol-specific
 * message. */
int parse_message(struct req_info *req,
                  const unsigned char *buf, size_t len)
{
    uint32_t hdr, ver, id;
    uint16_t cmd, flags;
    const unsigned char *payload;
    size_t psize;

    if (len < 17) {
        g_stat.net_broken_req++;
        req->reply_mini(req, REP_ERR_BROKEN);
        return 0;
    }

    MSG_DUMP("recv: ", buf, len);
    
    /* The header is:
     * 4 bytes    Version + ID
     * 2 bytes    Command
     * 2 bytes    Flags
     * Variable     Payload
     */

    hdr = * ((uint32_t *) buf);
    hdr = htonl(hdr);

    /* FIXME: little endian-only */
    ver = (hdr & 0xF0000000) >> 28;
    id = hdr & 0x0FFFFFFF;
    req->id = id;

    cmd = ntohs(* ((uint16_t *) buf + 2));
    flags = ntohs(* ((uint16_t *) buf + 3));

    if (ver != PROTO_VER) {
        g_stat.net_version_mismatch++;
        req->reply_mini(req, REP_ERR_VER);
        return 0;
    }

    /* We define payload as the stuff after buf. But be careful because
     * there might be none (if len == 1). Doing the pointer arithmetic
     * isn't problematic, but accessing the payload should be done only if
     * we know we have enough data. */
    payload = buf + 8;
    psize = len - 8;

    /* Store the id encoded in network byte order, so that we don't have
     * to calculate it at send time. */
    req->id = htonl(id);
    req->cmd = cmd;
    req->flags = flags;
    req->payload = payload;
    req->psize = psize;

    parse_event(req);

    return 1;
}
