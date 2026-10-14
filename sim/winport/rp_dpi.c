// Copyright 2026 The zynq-qemu-xsim-cosim authors
// SPDX-License-Identifier: Apache-2.0

/*
 * rp_dpi.c - minimal QEMU remote-port client for XSim over DPI-C.
 *
 * Plays the PL side of Xilinx's remote-port protocol (the same wire protocol
 * libsystemctlm-soc speaks) without SystemC/TLM, so it builds with xsc's DPI
 * flow (xsc + xelab -sv_lib) on Windows and Linux.
 *
 * Verilog drives one AXI4-Lite beat per rp_next()/rp_done() pair:
 *   rp_open()            connect, exchange hello
 *   rp_next(...)  -> 1   a 32-bit beat is ready (is_write/addr/wdata/wstrb)
 *                    2   nothing pending right now; advance sim time and retry
 *                    0   peer closed the connection; finish the simulation
 *   rp_done(rdata, ok)   complete that beat (the response is sent after the
 *                        last beat of the bus access)
 *
 * Only M_AXI_GP0-style single-stream bus accesses are handled; interrupt,
 * ATS and cfg packets are consumed and ignored (wire updates stay posted
 * because CAP_WIRE_POSTED_UPDATES is not advertised).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include "wsdyn.h"
#else
#include <sys/select.h>
#include <sys/time.h>
#endif

#include "remote-port-proto.h"
#include "remote-port-sk.h"
#include "safeio.h"

#define RX_MAX (64 * 1024)

static int fd = -1;
static struct rp_peer_state peer;
static uint32_t tx_id;
static unsigned char rx_buf[RX_MAX];
static unsigned char tx_buf[RX_MAX];

/* bus access currently being served beat by beat */
static int pend_valid;
static int pend_write;
static uint64_t pend_addr;
static uint32_t pend_len;
static uint32_t pend_off;
static unsigned char *pend_data;      /* write data inside rx_buf */
static unsigned char pend_rdata[4096];

static struct rp_pkt *rx_pkt(void) { return (struct rp_pkt *) rx_buf; }

static int rp_wr(const void *p, size_t n)
{
    return rp_safe_write(fd, p, n) == (ssize_t) n ? 0 : -1;
}

static int rp_rd(void *p, size_t n)
{
    return rp_safe_read(fd, p, n) == (ssize_t) n ? 0 : -1;
}

static int send_hello(void)
{
    uint32_t caps[] = { CAP_BUSACCESS_EXT_BASE };
    uint32_t caps_out[1];
    struct rp_pkt_hello pkt;
    size_t len;

    memset(&pkt, 0, sizeof pkt);
    len = rp_encode_hello_caps(tx_id++, 0, &pkt, RP_VERSION_MAJOR,
                               RP_VERSION_MINOR, caps, caps_out, 1);
    if (rp_wr(&pkt, len) || rp_wr(caps_out, sizeof caps_out))
        return -1;
    return 0;
}

int rp_open(const char *descr)
{
    if (!descr || !*descr)
        descr = getenv("RP_SOCKET");
    if (!descr || !*descr) {
        fprintf(stderr, "rp_open: RP_SOCKET is not set\n");
        return -1;
    }
    fd = sk_open(descr);
    if (fd < 0) {
        fprintf(stderr, "rp_open: cannot connect to %s\n", descr);
        return -1;
    }
    memset(&peer, 0, sizeof peer);
    if (send_hello()) {
        fprintf(stderr, "rp_open: hello failed\n");
        return -1;
    }
    return 0;
}

static int data_ready(int timeout_ms)
{
    fd_set rd;
    struct timeval tv;

    FD_ZERO(&rd);
    FD_SET((unsigned) fd, &rd);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return select(fd + 1, &rd, NULL, NULL, &tv) > 0;
}

static void send_busaccess_resp(int resp)
{
    struct rp_pkt *req = rx_pkt();
    struct rp_encode_busaccess_in in;
    struct rp_pkt *tx = (struct rp_pkt *) tx_buf;
    unsigned char *d;
    size_t plen;

    if (req->hdr.flags & RP_PKT_FLAGS_posted)
        return;                          /* posted write: no response */

    rp_encode_busaccess_in_rsp_init(&in, req);
    d = rp_busaccess_tx_dataptr(&peer, &tx->busaccess_ext_base);
    if (!pend_write)
        memcpy(d, pend_rdata, pend_len);
    in.clk = req->busaccess.timestamp;
    in.attr |= (uint64_t) resp << RP_BUS_RESP_SHIFT;
    plen = rp_encode_busaccess(&peer, &tx->busaccess_ext_base, &in);
    rp_wr(tx, plen);
}

static void handle_nonbus(struct rp_pkt *p)
{
    switch (p->hdr.cmd) {
    case RP_CMD_hello:
        if (p->hello.caps.len)
            rp_process_caps(&peer, (char *) p + p->hello.caps.offset,
                            p->hello.caps.len);
        break;
    case RP_CMD_sync: {
        struct rp_pkt_sync *r = (struct rp_pkt_sync *) tx_buf;
        size_t n = rp_encode_sync_resp(p->hdr.id, p->hdr.dev, r,
                                       (int64_t) p->sync.timestamp);
        rp_wr(r, n);
        break;
    }
    default:                             /* interrupt, cfg, ats...: ignore */
        break;
    }
}

/* Reads and dispatches one packet. Returns 1 if a bus access was queued. */
static int pump_one(void)
{
    struct rp_pkt *p = rx_pkt();
    uint32_t dlen;

    if (rp_rd(&p->hdr, sizeof p->hdr))
        return -1;
    rp_decode_hdr(p);
    if (p->hdr.len > RX_MAX - sizeof p->hdr)
        return -1;
    if (p->hdr.len && rp_rd((char *) &p->hdr + sizeof p->hdr, p->hdr.len))
        return -1;
    dlen = rp_decode_payload(p);

    if (p->hdr.flags & RP_PKT_FLAGS_response)
        return 0;                        /* we issue no requests */

    if (p->hdr.cmd == RP_CMD_write || p->hdr.cmd == RP_CMD_read) {
        pend_write = p->hdr.cmd == RP_CMD_write;
        pend_addr = p->busaccess.addr;
        pend_len = p->busaccess.len;
        pend_off = 0;
        pend_data = rx_buf + sizeof p->hdr + dlen;
        if (pend_len > sizeof pend_rdata)
            return -1;
        memset(pend_rdata, 0, sizeof pend_rdata);
        pend_valid = 1;
        return 1;
    }
    handle_nonbus(p);
    return 0;
}

int rp_next(long long now_ns, int idle_ms, int *addr, int *wdata, int *wstrb,
            int *is_write)
{
    (void) now_ns;

    for (;;) {
        if (pend_valid && pend_off < pend_len) {
            uint64_t a = pend_addr + pend_off;
            unsigned lane = (unsigned) (a & 3);
            unsigned n = 4 - lane;
            uint32_t w = 0;
            unsigned i;

            if (n > pend_len - pend_off)
                n = pend_len - pend_off;
            if (pend_write)
                for (i = 0; i < n; i++)
                    w |= (uint32_t) pend_data[pend_off + i] << (8 * (lane + i));
            *addr = (int) (a & ~3ULL);
            *wdata = (int) w;
            *wstrb = (int) (((1u << n) - 1u) << lane);
            *is_write = pend_write;
            return 1;
        }

        if (!data_ready(idle_ms))
            return 2;
        switch (pump_one()) {
        case 1:  break;                  /* loop again to emit first beat */
        case 0:  break;
        default: return 0;               /* EOF or protocol error */
        }
    }
}

void rp_done(int rdata, int ok)
{
    uint64_t a = pend_addr + pend_off;
    unsigned lane = (unsigned) (a & 3);
    unsigned n = 4 - lane;
    unsigned i;

    if (n > pend_len - pend_off)
        n = pend_len - pend_off;
    if (!pend_write)
        for (i = 0; i < n; i++)
            pend_rdata[pend_off + i] = (unsigned char) (((uint32_t) rdata) >> (8 * (lane + i)));
    pend_off += n;

    if (pend_off >= pend_len) {
        send_busaccess_resp(ok ? RP_RESP_OK : RP_RESP_BUS_GENERIC_ERROR);
        pend_valid = 0;
    }
}

void rp_close(void)
{
    if (fd >= 0) {
#ifdef _WIN32
        closesocket((SOCKET) fd);
#endif
        fd = -1;
    }
}
